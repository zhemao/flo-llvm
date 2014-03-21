/*
 * Copyright (C) 2014 Palmer Dabbelt
 *   <palmer.dabbelt@eecs.berkeley.edu>
 *
 * This file is part of flo-llvm.
 *
 * flo-llvm is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * flo-llvm is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with flo-llvm.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "flo.h++"
#include "node.h++"
#include "operation.h++"

#include "version.h"

#include <libcodegen/arglist.h++>
#include <libcodegen/builtin.h++>
#include <libcodegen/constant.h++>
#include <libcodegen/fix.h++>
#include <libcodegen/llvm.h++>
#include <libcodegen/op_alu.h++>
#include <libcodegen/op_bits.h++>
#include <libcodegen/op_call.h++>
#include <libcodegen/op_cond.h++>
#include <libcodegen/op_mem.h++>
#include <libcodegen/pointer.h++>
#include <libcodegen/vargs.h++>

#include <algorithm>
#include <string.h>
#include <string>
#include <map>

using namespace libcodegen;

#ifndef BUFFER_SIZE
#define BUFFER_SIZE 1024
#endif

enum gentype {
    GENTYPE_IR,
    GENTYPE_HEADER,
    GENTYPE_COMPAT,
    GENTYPE_ERROR,
};

/* These generate the different sorts of files that can be produced by
 * the C++ toolchain. */
static int generate_header(const flo_ptr flo, FILE *f);
static int generate_compat(const flo_ptr flo, FILE *f);
static int generate_llvmir(const flo_ptr flo, FILE *f);

/* Returns TRUE if the haystack starts with the needle. */
static bool strsta(const std::string haystack, const std::string needle);

int main(int argc, const char **argv)
{
    /* Prints the version if it was asked for. */
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        fprintf(stderr, "%s\n", PCONFIGURE_VERSION);
        exit(0);
    }

    /* Prints the help text if it was asked for or if there was no
     * input file given. */
    if (argc == 1 || argc > 3 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr, "%s: <flo> <type>\n", argv[0]);
        fprintf(stderr, "  Converts a Flo file to LLVM IR\n");
        fprintf(stderr, "  The output will be a drop-in replacement for\n");
        fprintf(stderr, "  Chisel's C++ emulator\n");
        exit(1);
    }

    /* An input filename of "-" means read from stdin. */
    auto infn = strcmp(argv[1], "-") == 0 ? "/dev/stdin" : argv[1];

    /* Look at the second argument to figure out what type to
     * generate. */
    enum gentype type = GENTYPE_ERROR;
    if (strcmp(argv[2], "--ir") == 0)
        type = GENTYPE_IR;
    if (strcmp(argv[2], "--header") == 0)
        type = GENTYPE_HEADER;
    if (strcmp(argv[2], "--compat") == 0)
        type = GENTYPE_COMPAT;

    /* Reads the input file and infers the width of every node. */
    auto flo = flo::parse(infn);

    /* Figures out what sort of output to generate. */
    switch (type) {
    case GENTYPE_IR:
        return generate_llvmir(flo, stdout);
    case GENTYPE_HEADER:
        return generate_header(flo, stdout);
    case GENTYPE_COMPAT:
        return generate_compat(flo, stdout);
    case GENTYPE_ERROR:
        fprintf(stderr, "Unknown generate target '%s'\n", argv[2]);
        fprintf(stderr, "  valid targets are:\n");
        fprintf(stderr, "    --ir:     Generates LLVM IR\n");
        fprintf(stderr, "    --header: Generates a C++ class header\n");
        fprintf(stderr, "    --compat: Generates a C++ compat layer\n");
        abort();
        return 1;
    }

    return 0;
}

int generate_header(const flo_ptr flo, FILE *f)
{
    /* Figures out the class name, printing that out. */
    fprintf(f, "#include <stdio.h>\n");
    fprintf(f, "#include <stdint.h>\n");
    /* FIXME: Don't depend on Chisel's emulator.h, it kind of
     * defeats the point of doing all this in the first
     * place... */
    fprintf(f, "#include \"emulator.h\"\n");
    fprintf(f, "class %s_t: public mod_t {\n", flo->class_name().c_str());
    fprintf(f, "  public:\n");

    /* Declares the variables that need to be present in the C++
     * header file in order to maintain compatibility with Chisel's
     * output. */
    for (auto it = flo->nodes(); !it.done(); ++it) {
        auto node = *it;

        if (node->exported() == false)
            continue;

        if (node->is_mem() == true) {
            fprintf(f, "    mem_t<%lu, %lu> %s;\n",
                    node->width(),
                    node->depth(),
                    node->mangled_name().c_str());
        } else {
            fprintf(f, "    dat_t<%lu> %s;\n",
                    node->width(),
                    node->mangled_name().c_str());

            fprintf(f, "    dat_t<%lu> %s__prev;\n",
                    node->width(),
                    node->mangled_name().c_str());
        }
    }

    /* Here we write the class methods that exist in Chisel and will
     * be implemented externally by either the compatibility layer or
     * the emitted LLVM IR.  These must exactly match the
     * Chisel-emitted definitions. */
    fprintf(f, "  public:\n");
    fprintf(f, "    void init(bool random_init = false);\n");
    fprintf(f, "    int clock(dat_t<1> reset);\n");
    fprintf(f, "    void clock_lo(dat_t<1> reset);\n");
    fprintf(f, "    void clock_hi(dat_t<1> reset);\n");
    fprintf(f, "    void dump(FILE *file, int clock);\n");

    /* Close the class */
    fprintf(f, "};\n");

    /* The new Chisel emulator appears to require a second class
     * that's used for debug info. */
    fprintf(f, "class %s_api_t : public mod_api_t {\n",
            flo->class_name().c_str());
    fprintf(f, "  void init_mapping_table(void);\n");
    fprintf(f, "};\n");

    return 0;
}

int generate_compat(const flo_ptr flo, FILE *f)
{
    /* The whole point of this is to work around the C++ name
     * mangling. */
    fprintf(f, "extern \"C\" {\n");

    /* Produce accessor functions that can be used to get pointers to
     * particular fields within the C++ class definition.  The idea
     * here is that I can get around C++ name mangling by exporting
     * these as C names. */
    for (auto it = flo->nodes(); !it.done(); ++it) {
        auto node = *it;

        if (node->exported() == false)
            continue;

        if (node->is_mem() == true) {
            /* FIXME: How do I emit these? */
        } else {
            /* This function pulls the value from a node into an
             * array.  Essentially this just does C++ name
             * demangling. */
            fprintf(f, "  void _llvmflo_%s_get(%s_t *d, uint64_t *a) {\n",
                    node->mangled_name().c_str(),
                    flo->class_name().c_str()
                );

            for (size_t i = 0; i < (node->width() + 63) / 64; ++i) {
                fprintf(f, "    a[%lu] = d->%s.values[%lu];\n",
                        i,
                        node->mangled_name().c_str(),
                        i
                    );
            }

            fprintf(f, "  }\n");

            /* The opposite of the above: sets a dat_t value. */
            fprintf(f, "  void _llvmflo_%s_set(%s_t *d, uint64_t *a) {\n",
                    node->mangled_name().c_str(),
                    flo->class_name().c_str()
                );

            for (size_t i = 0; i < (node->width() + 63) / 64; ++i) {
                fprintf(f, "    d->%s.values[%lu] = a[%lu];\n",
                        node->mangled_name().c_str(),
                        i,
                        i
                    );
            }

            fprintf(f, "  }\n");
        }
    }

    /* Here's where we elide the last bits of name mangling: these
     * functions wrap some non-mangled IR-generated names that
     * actually implement the functions required by Chisel's C++
     * interface. */
    fprintf(f, "  void _llvmflo_%s_init(%s_t *p, bool r);\n",
            flo->class_name().c_str(), flo->class_name().c_str());

    fprintf(f, "  void _llvmflo_%s_clock_lo(%s_t *p, bool r);\n",
            flo->class_name().c_str(), flo->class_name().c_str());

    fprintf(f, "  void _llvmflo_%s_clock_hi(%s_t *p, bool r);\n",
            flo->class_name().c_str(), flo->class_name().c_str());

    /* End the 'extern "C"' block above. */
    fprintf(f, "};\n");

    /* The clock function just calls the other two clock functions. */
    fprintf(f, "int %s_t::clock(dat_t<1> rd)\n", flo->class_name().c_str());
    fprintf(f, "  { clock_lo(rd); clock_hi(rd); return 0; }\n");

    /* Actually define the (non mangled) implementation of the Chisel
     * C++ interface, which in fact only calls the LLVM-generated
     * functions. */
    fprintf(f, "void %s_t::clock_lo(dat_t<1> rd)\n",
            flo->class_name().c_str());
    fprintf(f, "  { _llvmflo_%s_clock_lo(this, rd.to_ulong()); }\n",
            flo->class_name().c_str());

    /* init just sets everything to zero, which is easy to do in C++
     * (it'll be fairly short). */
    fprintf(f, "void %s_t::init(bool r)\n{\n", flo->class_name().c_str());
    for (auto it = flo->nodes(); !it.done(); ++it) {
        auto node = *it;

        if (node->exported() == false)
            continue;

        if (node->is_mem() == true) {
            /* FIXME: Do we initailize memories? */
        } else {
            fprintf(f, "  this->%s = 0;\n", node->mangled_name().c_str());
        }
    }
    fprintf(f, "}\n");

    /* clock_hi just copies data around and therefor is simplest to
     * stick in C++ -- using LLVM IR doesn't really gain us anything
     * here. */
    fprintf(f, "void %s_t::clock_hi(dat_t<1> rd)\n{\n",
            flo->class_name().c_str());
    fprintf(f, "  bool r = rd.to_ulong();\n");
    for (auto it = flo->operations(); !it.done(); ++it) {
        auto op = *it;

        /* Only registers need to be copied on */
        if (op->op() != libflo::opcode::REG)
            continue;

        fprintf(f, "  %s = %s;\n",
                op->d()->mangled_name().c_str(),
                op->t()->mangled_name().c_str()
            );
    }
    fprintf(f, "}\n");

    /* VCD dumping is implemented directly in C++ here because I don't
     * really see a reason not to. */
    fprintf(f, "void %s_t::dump(FILE *f, int cycle)\n{\n",
            flo->class_name().c_str());

    /* On the first cycle we need to write out the VCD header file. */
    fprintf(f, "  if (cycle == 0) {\n");
    fprintf(f, "    fprintf(f, \"$timescale 1ps $end\\n\");\n");
    
    std::string last_path = "";
    for (auto it = flo->nodes_alpha(); !it.done(); ++it) {
        auto node = *it;

        char buffer[BUFFER_SIZE];
        snprintf(buffer, BUFFER_SIZE, "%s", node->name().c_str());

        /* Here's where we figure out where in the module heirarchy
         * this node is. */
        char *module = buffer;
        char *signal = buffer;
        for (size_t i = 0; i < strlen(buffer); i++)
            if (buffer[i] == ':')
                signal = buffer + i;

        /* These have no "::" in them, which means they're not
         * globally visible. */
        if (module == signal)
            continue;

        /* The module seperator can be either ":" or "::".  Detect
         * which one is actually generated and demangle the name
         * correctly. */
        if (signal[-1] == ':')
            signal[-1] = '\0';
        signal[0] = '\0';
        signal++;

        /* Figure out if we're going up or down a module and perform
         * that move. */
        if (strcmp(module, last_path.c_str()) == 0) {
        } else if (strsta(last_path, module)) {
            fprintf(f, "    fprintf(f, \"$upscope $end\\n\");\n");
        } else if (strsta(module, last_path)) {
            /* Determine a slightly shorter name for the module, which
             * is what VCD uses.  This is just the last component of
             * the module name, the remainder can be determined by the
             * hierarchy. */
            char *lastmodule = module;
            for (size_t i = 0; i < strlen(module); i++)
                if (module[i] == ':')
                    lastmodule = module + i;
            if (*lastmodule == ':')
                lastmodule++;

            fprintf(f, "    fprintf(f, \"$scope module %s $end\\n\");\n",
                    lastmodule);
        } else {
            fprintf(f, "    fprintf(f, \"$upscope $end\\n\");\n");

            /* Determine a slightly shorter name for the module, which
             * is what VCD uses.  This is just the last component of
             * the module name, the remainder can be determined by the
             * hierarchy. */
            char *lastmodule = module;
            for (size_t i = 0; i < strlen(module); i++)
                if (module[i] == ':')
                    lastmodule = module + i;
            if (*lastmodule == ':')
                lastmodule++;

            fprintf(f, "    fprintf(f, \"$scope module %s $end\\n\");\n",
                    lastmodule);
        }

        /* After changing modules, go ahead and output the wire. */
        fprintf(f, "    fprintf(f, \"$var wire %lu %s %s $end\\n\");\n",
                node->width(),
                node->vcd_name().c_str(),
                signal
            );

        /* The last path is always equal to the current one -- note
         * that sometimes this won't do anything as it'll be the same,
         * but this strictly enforces this condition. */
        last_path = module;
    }

    size_t colon_count = 0;
    for (size_t i = 0; i < strlen(last_path.c_str()); i++)
        if (last_path[i] == ':')
            colon_count++;

    for (size_t i = 0; i <= (colon_count / 2); i++)
        fprintf(f, "    fprintf(f, \"$upscope $end\\n\");\n");

    fprintf(f, "  fprintf(f, \"$enddefinitions $end\\n\");\n");
    fprintf(f, "  fprintf(f, \"$dumpvars\\n\");\n");
    fprintf(f, "  fprintf(f, \"$end\\n\");\n");

    fprintf(f, "  }\n");

    fprintf(f, "  fprintf(f, \"#%%lu\\n\", cycle);\n");

    for (auto it = flo->nodes(); !it.done(); ++it) {
        auto node = *it;

        if (node->vcd_exported() == false)
            continue;

        fprintf(f,
                "  if ((cycle == 0) || (%s__prev != %s).to_ulong()) {\n",
                node->mangled_name().c_str(),
                node->mangled_name().c_str()
            );

        fprintf(f, "    dat_dump(f, %s, \"%s\");\n",
                node->mangled_name().c_str(),
                node->vcd_name().c_str()
            );

        fprintf(f, "    %s__prev = %s;\n",
                node->mangled_name().c_str(),
                node->mangled_name().c_str()
            );

        fprintf(f, "  }\n");
    }

    fprintf(f, "}\n");

    /* This function is part of the debug API wrapper, which now
     * contains all the string-lookup stuff. */
    fprintf(f, "void %s_api_t::init_mapping_table(void) {\n",
            flo->class_name().c_str());

    fprintf(f, "  dat_table.clear();\n");
    fprintf(f, "  mem_table.clear();\n");
    fprintf(f, "  %s_t *dut = dynamic_cast<%s_t*>(module);\n",
            flo->class_name().c_str(),
            flo->class_name().c_str()
        );
    fprintf(f, "  if (dut == NULL) {assert(dut != NULL); abort();}\n");

    for (auto it = flo->nodes(); !it.done(); ++it) {
        auto node = *it;

        if (node->exported() == false)
            continue;

        if (node->is_mem() == true) {
            fprintf(f, "  mem_table[\"%s\"] = new mem_api<%lu, %lu>(&dut->%s, \"%s\", \"\");\n",
                    node->chisel_name().c_str(),
                    node->width(),
                    node->depth(),
                    node->mangled_name().c_str(),
                    node->chisel_name().c_str()
                );
        } else {
            fprintf(f, "  dat_table[\"%s\"] = new dat_api<%lu>(&dut->%s, \"%s\", \"\");\n",
                    node->chisel_name().c_str(),
                    node->width(),
                    node->mangled_name().c_str(),
                    node->chisel_name().c_str()
                );
        }
        
    }

    fprintf(f, "}\n");

    return 0;
}

int generate_llvmir(const flo_ptr flo, FILE *f)
{
    /* This writer outputs LLVM IR to the given file. */
    llvm out(f);

    /* Generate declarations for some external functions that get used
     * by generated code below. */
    function< builtin<void>,
              arglist2< pointer< builtin<char> >,
                        vargs
                        >
              >
        extern_printf("printf");
    out.declare(extern_printf);

    function< builtin<void>,
              arglist5< pointer< builtin<char> >,
                        builtin<char>,
                        builtin<uint64_t>,
                        builtin<uint32_t>,
                        builtin<bool>
                        >
              >
        extern_memset("llvm.memset.p0i8.i64");
    out.declare(extern_memset);

    /* These symbols are generated by the compatibility layer but
     * still need declarations so LLVM can check their types.  Note
     * that here I'm just manually handling this type safety, which is
     * probably nasty... */
    for (auto it = flo->nodes(); !it.done(); ++it) {
        auto node = *it;

        if (node->exported() == false)
            continue;

        if (node->is_mem() == true) {
            /* FIXME: Should I bother emiting these? */
        } else {
            out.declare(node->get_func());
            out.declare(node->set_func());
        }
    }

    /* Here we generate clock_lo, which performs all the logic
     * operations but does not perform any register writes.  In order
     * to do this we'll have to walk through the computation in
     * dataflow order. */
    function< builtin<void>,
              arglist2<pointer<builtin<void>>,
                       builtin<bool>
                       >
              >
        clock_lo("_llvmflo_%s_clock_lo", flo->class_name().c_str());
    {
        auto dut = pointer<builtin<void>>("dut");
        auto dut_vec = std::vector<value*>();
        dut_vec.push_back(&dut);

        auto rst = builtin<bool>("rst");
        auto rst_vec = std::vector<value*>();
        rst_vec.push_back(&rst);

        auto lo = out.define(clock_lo, {&dut, &rst});

        /* The code is already in dataflow order so all we need to do
         * is emit the computation out to LLVM. */
        for (auto it = flo->operations(); !it.done(); ++it) {
            auto op = *it;

            /* This contains a count of the number of i64-wide
             * operations that need to be performed in order to make
             * this operation succeed. */
            auto i64cnt = constant<uint32_t>((op->d()->width() + 63) / 64);

            lo->comment("");
            lo->comment(" *** Chisel Node: %s", op->to_string().c_str());
            lo->comment("");

            bool nop = false;
            switch (op->op()) {
                /* The following nodes are just no-ops in this phase, they
                 * only show up in the clock_hi phase. */
            case libflo::opcode::OUT:
                lo->operate(mov_op(op->dv(), op->sv()));
                break;

            case libflo::opcode::ADD:
                lo->operate(add_op(op->dv(), op->sv(), op->tv()));
                break;

            case libflo::opcode::AND:
                lo->operate(and_op(op->dv(), op->sv(), op->tv()));
                break;

            case libflo::opcode::CAT:
            case libflo::opcode::CATD:
            {
                auto se = fix_t(op->d()->width());
                auto te = fix_t(op->d()->width());
                lo->operate(zero_ext_op(se, op->sv()));
                lo->operate(zero_ext_op(te, op->tv()));

                auto ss = fix_t(op->d()->width());
                lo->operate(lsh_op(ss, se, constant<uint64_t>(op->width())));

                lo->operate(or_op(op->dv(), te, ss));

                break;
            }

            case libflo::opcode::EQ:
                lo->operate(cmp_eq_op(op->dv(), op->sv(), op->tv()));
                break;

            case libflo::opcode::GTE:
                lo->operate(cmp_gte_op(op->dv(), op->sv(), op->tv()));
                break;

            case libflo::opcode::LT:
                lo->operate(cmp_lt_op(op->dv(), op->sv(), op->tv()));
                break;

            case libflo::opcode::LSH:
            {
                auto es = fix_t(op->d()->width());
                auto et = fix_t(op->d()->width());

                lo->operate(zero_ext_op(es, op->sv()));
                lo->operate(zero_ext_op(et, op->tv()));
                
                lo->operate(lsh_op(op->dv(), es, et));

                break;
            }

            case libflo::opcode::MOV:
                lo->operate(mov_op(op->dv(), op->sv()));
                break;

            case libflo::opcode::MUL:
            {
                auto ext0 = fix_t(op->d()->width());
                auto ext1 = fix_t(op->d()->width());

                lo->operate(zero_ext_op(ext0, op->sv()));
                lo->operate(zero_ext_op(ext1, op->tv()));
                lo->operate(mul_op(op->dv(), ext0, ext1));
                break;
            }

            case libflo::opcode::MUX:
                lo->operate(mux_op(op->dv(),
                                   op->sv(),
                                   op->tv(),
                                   op->uv()
                                ));
                break;

            case libflo::opcode::NEQ:
                lo->operate(cmp_neq_op(op->dv(), op->sv(), op->tv()));
                break;

            case libflo::opcode::NOT:
                lo->operate(not_op(op->dv(), op->sv()));
                break;

            case libflo::opcode::OR:
                lo->operate(or_op(op->dv(), op->sv(0), op->sv(1)));
                break;

            case libflo::opcode::IN:
            case libflo::opcode::REG:
            {
                nop = true;

                auto ptr64 = pointer<builtin<uint64_t>>();
                auto ptrC = pointer<builtin<void>>();

                /* Obtain a pointer to the C++ structure's internal
                 * structure definiton so it can be converted into an
                 * LLVM operation. */
                lo->operate(alloca_op(ptr64, i64cnt));
                lo->operate(call_op(op->d()->get_func(), {&dut, &ptr64}));

                /* Here we generate the internal temporary values.
                 * This series of shift/add operations will probably
                 * be compiled into NOPs by the LLVM optimizer. */
                auto ptrs = std::vector<pointer<builtin<uint64_t>>>(i64cnt);
                for (size_t i = 0; i < i64cnt; i++) {
                    auto index = constant<size_t>(i);
                    lo->operate(index_op(ptrs[i], ptr64, index));
                }

                auto loads = std::vector<builtin<uint64_t>>(i64cnt);
                for (size_t i = 0; i < i64cnt; ++i) {
                    lo->operate(load_op(loads[i], ptrs[i]));
                }

                auto extended = std::vector<fix_t>();
                for (size_t i = 0; i < i64cnt; ++i) {
                    /* We need this push here because every one of
                     * these temporaries needs a new name which means
                     * the copy constructor can't be used.  The
                     * default constructor can't be used because we
                     * need to tag each fix with a width. */
                    extended.push_back(fix_t(op->width()));
                    lo->operate(zext_trunc_op(extended[i], loads[i]));
                }

                auto shifted = std::vector<fix_t>();
                for (size_t i = 0; i < i64cnt; i++) {
                    shifted.push_back(fix_t(op->width()));
                    auto offset = constant<uint32_t>(i * 64);
                    lo->operate(lsh_op(shifted[i], extended[i], offset));
                }

                auto ored = std::vector<fix_t>();
                for (size_t i = 0; i < i64cnt; ++i) {
                    ored.push_back(fix_t(op->width()));
                    if (i == 0) {
                        lo->operate(mov_op(ored[i], shifted[i]));
                    } else {
                        lo->operate(or_op(ored[i], shifted[i], ored[i-1]));
                    }
                }

                lo->operate(mov_op(op->dv(), ored[i64cnt-1]));

                break;
            }

            case libflo::opcode::RSH:
            {
                auto cast = fix_t(op->s()->width());
                lo->operate(zext_trunc_op(cast, op->tv()));

                auto shifted = fix_t(op->s()->width());
                lo->operate(lrsh_op(shifted, op->sv(), cast));
                lo->operate(zext_trunc_op(op->dv(), shifted));
                break;
            }

            case libflo::opcode::RST:
                lo->operate(unsafemov_op(op->dv(), rst));
                break;

            case libflo::opcode::SUB:
                lo->operate(sub_op(op->dv(), op->sv(), op->tv()));
                break;

            case libflo::opcode::XOR:
                lo->operate(xor_op(op->dv(), op->sv(0), op->tv()));
                break;

            case libflo::opcode::RND:
            case libflo::opcode::EAT:
            case libflo::opcode::LIT:
            case libflo::opcode::MSK:
            case libflo::opcode::LD:
            case libflo::opcode::ARSH:
            case libflo::opcode::ST:
            case libflo::opcode::MEM:
            case libflo::opcode::NOP:
            case libflo::opcode::LOG2:
            case libflo::opcode::NEG:
            case libflo::opcode::RD:
            case libflo::opcode::WR:
                fprintf(stderr, "Unable to compute node '%s'\n",
                        libflo::opcode_to_string(op->op()).c_str());
                abort();
                break;
            }

            /* Every node that's in the Chisel header gets stored after
             * its cooresponding computation, but only when the node
             * appears in the Chisel header. */
            if (op->writeback() == true && nop == false) {
                lo->comment("  Writeback");

                /* This generates a pointer that can be passed to C++,
                 * in other words, an array-of-uints. */
                auto ptr64 = pointer<builtin<uint64_t>>();
                lo->operate(alloca_op(ptr64, i64cnt));

                /* Here we generate the internal temporary values.
                 * This series of shift/add operations will probably
                 * be compiled into NOPs by the LLVM optimizer. */
                auto shifted = std::vector<fix_t>();
                for (size_t i = 0; i < i64cnt; ++i) {
                    shifted.push_back(fix_t(op->d()->width()));
                    auto offset = constant<uint32_t>(i * 64);
                    lo->operate(lrsh_op(shifted[i], op->dv(), offset));
                }

                auto trunced = std::vector<builtin<uint64_t>>(i64cnt);
                for (size_t i = 0; i < i64cnt; ++i) {
                    lo->operate(zext_trunc_op(trunced[i], shifted[i]));
                }

                auto ptrs = std::vector<pointer<builtin<uint64_t>>>(i64cnt);
                for (size_t i = 0; i < i64cnt; ++i) {
                    auto index = constant<size_t>(i);
                    lo->operate(index_op(ptrs[i], ptr64, index));
                }

                for (size_t i = 0; i < i64cnt; ++i) {
                    lo->operate(store_op(ptrs[i], trunced[i]));
                }

                /* Here we fetch the actual C++ pointer that can be
                 * used to move this signal's data out. */
                auto ptrC = pointer<builtin<void>>();
                lo->operate(call_op(op->d()->set_func(), {&dut, &ptr64}));
            }
        }

        fprintf(f, "  ret void\n");
    }

    return 0;
}

bool strsta(const std::string haystack, const std::string needle)
{
    const char *h = haystack.c_str();
    const char *n = needle.c_str();

    return (strncmp(h, n, strlen(n)) == 0);
}
