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

#include "llvm.h++"
using namespace libcodegen;

/* Converts a name to an LLVM name.  Essentially this means one of two
 * things: if it's an integer constant then do nothing, otherwise put
 * a '%L' before the name. */
static const std::string llvm_name(const std::string name);

llvm::llvm(FILE *f)
    : _f(f)
{
}

llvm::llvm(const std::string filename)
    : _f(fopen(filename.c_str(), "w"))
{
}

void llvm::declare(const function_t &f)
{
    fprintf(_f, "declare %s @%s(", f.ret_llvm().c_str(), f.name().c_str());

    auto args = f.args_llvm();
    if (args.size() > 0) {
        size_t i = 0;
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (i != 0)
                fprintf(_f, ", ");
            i++;

            fprintf(_f, "%s", (*it).c_str());
        }
    }

    fprintf(_f, ")\n");
}

definition_ptr llvm::define(const function_t &f,
                            const std::vector<std::string> &arg_names)
{
    if (f.args_llvm().size() != arg_names.size()) {
        fprintf(stderr, "Mismatched args and names sizes\n");
        abort();
    }

    fprintf(_f, "define %s @%s(", f.ret_llvm().c_str(), f.name().c_str());

    auto args = f.args_llvm();
    if (args.size() > 0) {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i != 0)
                fprintf(_f, ", ");

            fprintf(_f, "%s %%%s", args[i].c_str(), arg_names[i].c_str());
        }
    }

    fprintf(_f, ")\n{\n");

    return definition_ptr(new definition(this));
}

void llvm::comment(const std::string format, ...)
{
    va_list args;
    va_start(args, format);
    comment(format, args);
    va_end(args);
}

void llvm::comment(const std::string format, va_list args)
{
    fprintf(_f, "  ; ");
    vfprintf(_f, format.c_str(), args);
    fprintf(_f, "\n");
}

void llvm::operate(const operation &op)
{
    fprintf(_f, "  %s = %s %s %s, %s\n",
            llvm_name(op.d().name()).c_str(),
            op.op_llvm().c_str(),
            op.s0().as_llvm().c_str(),
            llvm_name(op.s0().name()).c_str(),
            llvm_name(op.s1().name()).c_str()
        );
}

void llvm::define_finish(const definition *d __attribute__((unused)))
{
    fprintf(_f, "}\n\n");
}

/* FIXME: This should be prefixed with '%L', not '%C__'.  It's done
 * this way to match the old name mangling. */
const std::string llvm_name(const std::string name)
{
    if (isdigit(name.c_str()[0]))
        return name;

    char buffer[1024];
    snprintf(buffer, 1024, "%%C__%s", name.c_str());
    return buffer;
}
