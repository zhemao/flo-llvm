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

#ifndef NODE_HXX
#define NODE_HXX

#include <libflo/node.h++>
#include <memory>
#include <vector>

class node;
typedef std::shared_ptr<node> node_ptr;

/* This defines our extension of a node.  The idea here is to provide
 * some semblance of type safety when generating code by looking up
 * values within this header file as opposed to attempting to generate
 * coherent values in many places. */
class node: public libflo::node {
private:
public:
    /* Fills out this node with the extra information that's needed in
     * order to make code generation work. */
    node(const libflo::node_ptr n);
};

#endif
