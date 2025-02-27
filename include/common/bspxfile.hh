/*  Copyright (C) 1996-1997  Id Software, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#pragma once

#include <array>
#include <iosfwd>
#include <cstdint>
#include <common/aabb.hh>
#include <memory>
#include <common/bspfile.hh>

/* ========================================================================= */

struct bspx_header_t
{
    std::array<char, 4> id = {'B', 'S', 'P', 'X'}; //'BSPX'
    uint32_t numlumps;

    bspx_header_t() = default;
    bspx_header_t(uint32_t numlumps);

    // serialize for streams
    void stream_write(std::ostream &s) const;
    void stream_read(std::istream &s);
};

struct bspx_lump_t
{
    std::array<char, 24> lumpname{};
    uint32_t fileofs;
    uint32_t filelen;

    // serialize for streams
    void stream_write(std::ostream &s) const;
    void stream_read(std::istream &s);
};

// BRUSHLIST BSPX lump
struct bspxbrushes_permodel
{
    int32_t ver;
    int32_t modelnum;
    int32_t numbrushes;
    int32_t numfaces;

    // serialize for streams
    void stream_write(std::ostream &s) const;
    void stream_read(std::istream &s);
};

struct bspxbrushes_perbrush
{
    aabb3f bounds;
    int16_t contents;
    uint16_t numfaces;

    // serialize for streams
    void stream_write(std::ostream &s) const;
    void stream_read(std::istream &s);
};

using bspxbrushes_perface = qplane3f;

// FACENORMALS BSPX lump
// struct isn't actually used, but provides technical
// specs of the lump
struct bspxfacenormals_header
{
    // num unique normals
    uint32_t num_normals;
    qvec3f *normals; // [num_normals]

    // the vertex data is stored per face, because vertex data
    // in the BSP is shared between faces; this won't allow for mixed
    // smoothing groups to work properly since they can't share normals.
    // if the BSP has 4 faces with 4 vertices each, then what follows is
    // 4 * 4 * (3 uint32_t); normal/tangent/bitangent per vertex per face.
    // for each bsp face:
    // for each face vertex:
    uint32_t normal;
    uint32_t tangent;
    uint32_t bitangent;
};

// DECOUPLED_LM BSPX lump (subject to change!)
struct bspx_decoupled_lm_perface
{
    uint16_t lmwidth; // pixels
    uint16_t lmheight; // pixels
    // offset into dlightdata lump.
    // start of numstyles (from face struct) * (lmwidth * lmheight) samples
    int32_t offset;

    // 2 rows * 4 column matrix, stored in row major order
    // this is a world -> lightmap space transformation matrix
    texvecf world_to_lm_space;

    // serialize for streams
    void stream_write(std::ostream &s) const;
    void stream_read(std::istream &s);
};

// BSPX data
