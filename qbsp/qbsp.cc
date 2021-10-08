/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

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

#include <memory>
#include <cstring>
#include <algorithm>

#include <common/log.hh>
#include <common/aabb.hh>
#include <qbsp/qbsp.hh>
#include <qbsp/wad.hh>
#include <fmt/chrono.h>

#include "tbb/global_control.h"

constexpr const char *IntroString = "---- qbsp / ericw-tools " stringify(ERICWTOOLS_VERSION) " ----\n";

// command line flags
options_t options;

bool node_t::opaque() const
{
    return contents.is_sky(options.target_game) || contents.is_solid(options.target_game);
}

// a simple tree structure used for leaf brush
// compression.
struct leafbrush_entry_t
{
    uint32_t offset;
    std::map<uint32_t, leafbrush_entry_t> entries;
};

// per-entity
static struct
{
    uint32_t total_brushes, total_brush_sides;
    uint32_t total_leaf_brushes;
} brush_state;

// running total
static uint32_t brush_offset;

static void ExportBrushList_r(const mapentity_t *entity, node_t *node, const uint32_t &brush_offset)
{
    if (node->planenum == PLANENUM_LEAF) {
        if (node->contents.native) {
            uint32_t b_id = brush_offset;
            std::vector<uint32_t> brushes;

            for (const brush_t *b = entity->brushes; b; b = b->next, b_id++) {
                if (node->bounds.intersectWith(b->bounds).valid) {
                    brushes.push_back(b_id);
                }
            }

            if (brushes.size()) {
                node->numleafbrushes = brushes.size();
                brush_state.total_leaf_brushes += node->numleafbrushes;
                node->firstleafbrush = map.bsp.dleafbrushes.size();
                map.bsp.dleafbrushes.insert(map.bsp.dleafbrushes.end(), brushes.begin(), brushes.end());
            }
        }

        return;
    }

    ExportBrushList_r(entity, node->children[0], brush_offset);
    ExportBrushList_r(entity, node->children[1], brush_offset);
}

/*
==============
SnapVector
==============
*/
static void SnapVector(vec3_t normal)
{
    int32_t i;

    for (i = 0; i < 3; i++) {
        if (fabs(normal[i] - 1) < NORMAL_EPSILON) {
            VectorClear(normal);
            normal[i] = 1;
            break;
        }
        if (fabs(normal[i] - -1) < NORMAL_EPSILON) {
            VectorClear(normal);
            normal[i] = -1;
            break;
        }
    }
}

/*
=================
AddBrushBevels

Adds any additional planes necessary to allow the brush to be expanded
against axial bounding boxes
=================
*/
static std::vector<std::tuple<size_t, face_t *>> AddBrushBevels(const brush_t *b)
{
    // add already-present planes
    std::vector<std::tuple<size_t, face_t *>> planes;

    for (face_t *f = b->faces; f; f = f->next) {
        int32_t planenum = f->planenum;

        if (f->planeside) {
            auto flipped = map.planes[f->planenum];
            flipped.dist = -flipped.dist;
            VectorInverse(flipped.normal);
            planenum = FindPlane(&flipped.normal[0], flipped.dist, nullptr);
        }

        int32_t outputplanenum = ExportMapPlane(planenum);
        planes.emplace_back(outputplanenum, f);
    }

    //
    // add the axial planes
    //
    int32_t order = 0;
    for (int32_t axis = 0; axis < 3; axis++) {
        for (int32_t dir = -1; dir <= 1; dir += 2, order++) {
            size_t i;
            // see if the plane is allready present
            for (i = 0; i < planes.size(); i++) {
                if (map.bsp.dplanes[std::get<0>(planes[i])].normal[axis] == dir)
                    break;
            }

            if (i == planes.size()) {
                // add a new side
                plane_t new_plane;
                VectorClear(new_plane.normal);
                new_plane.normal[axis] = dir;
                if (dir == 1)
                    new_plane.dist = b->bounds.maxs()[axis];
                else
                    new_plane.dist = -b->bounds.mins()[axis];

                int32_t planenum = FindPlane(&new_plane.normal[0], new_plane.dist, nullptr);
                int32_t outputplanenum = ExportMapPlane(planenum);
                planes.emplace_back(outputplanenum, nullptr);
            }

            // if the plane is not in it canonical order, swap it
            if (i != order)
                std::swap(planes[i], planes[order]);
        }
    }

    //
    // add the edge bevels
    //
    if (planes.size() == 6)
        return planes; // pure axial

    // test the non-axial plane edges
    size_t edges_to_test = planes.size();
    for (size_t i = 6; i < edges_to_test; i++) {
        auto &s = std::get<1>(planes[i]);
        if (!s)
            continue;
        auto &w = s->w;
        if (!w.size())
            continue;
        for (size_t j = 0; j < w.size(); j++) {
            vec3_t vec;
            size_t k = (j + 1) % w.size();
            VectorSubtract(w[j], w[k], vec);
            if (VectorNormalize(vec) < 0.5)
                continue;
            SnapVector(vec);
            for (k = 0; k < 3; k++)
                if (vec[k] == -1 || vec[k] == 1)
                    break; // axial
            if (k != 3)
                continue; // only test non-axial edges

            // try the six possible slanted axials from this edge
            for (int32_t axis = 0; axis < 3; axis++) {
                for (int32_t dir = -1; dir <= 1; dir += 2) {
                    vec3_t vec2;
                    // construct a plane
                    VectorClear(vec2);
                    vec2[axis] = dir;
                    plane_t current;
                    CrossProduct(vec, vec2, current.normal);
                    if (VectorNormalize(current.normal) < 0.5)
                        continue;
                    current.dist = DotProduct(w[j], current.normal);

                    face_t *f;

                    // if all the points on all the sides are
                    // behind this plane, it is a proper edge bevel
                    for (f = b->faces; f; f = f->next) {
                        auto &plane = map.planes[f->planenum];
                        plane_t temp;
                        VectorCopy(plane.normal, temp.normal);
                        temp.dist = plane.dist;

                        if (f->planeside) {
                            temp.dist = -temp.dist;
                            VectorInverse(temp.normal);
                        }

                        // if this plane has allready been used, skip it
                        if (PlaneEqual(&current, &temp))
                            break;

                        auto &w2 = f->w;
                        if (!w2.size())
                            continue;
                        size_t l;
                        for (l = 0; l < w2.size(); l++) {
                            vec_t d = DotProduct(w2[l], current.normal) - current.dist;
                            if (d > 0.1)
                                break; // point in front
                        }
                        if (l != w2.size())
                            break;
                    }

                    if (f)
                        continue; // wasn't part of the outer hull

                    // add this plane
                    int32_t planenum = FindPlane(&current.normal[0], current.dist, nullptr);
                    int32_t outputplanenum = ExportMapPlane(planenum);
                    planes.emplace_back(outputplanenum, nullptr);
                }
            }
        }
    }

    return planes;
}

static void ExportBrushList(const mapentity_t *entity, node_t *node, uint32_t &brush_offset)
{
    LogPrint(LOG_PROGRESS, "---- {} ----\n", __func__);

    brush_state = {};

    for (const brush_t *b = entity->brushes; b; b = b->next) {
        dbrush_t &brush = map.bsp.dbrushes.emplace_back(
            dbrush_t{static_cast<int32_t>(map.bsp.dbrushsides.size()), 0, b->contents.native});

        auto bevels = AddBrushBevels(b);

        for (auto &plane : bevels) {
            map.bsp.dbrushsides.push_back(
                {(uint32_t)std::get<0>(plane), (int32_t)ExportMapTexinfo(b->faces->texinfo)});
            brush.numsides++;
            brush_state.total_brush_sides++;
        }

        brush_state.total_brushes++;
    }

    ExportBrushList_r(entity, node, brush_offset);

    brush_offset += brush_state.total_brushes;

    LogPrint(LOG_STAT, "     {:8} total brushes\n", brush_state.total_brushes);
    LogPrint(LOG_STAT, "     {:8} total brush sides\n", brush_state.total_brush_sides);
    LogPrint(LOG_STAT, "     {:8} total leaf brushes\n", brush_state.total_leaf_brushes);
}

/*
=========================================================

FLOOD AREAS

=========================================================
*/

int32_t c_areas;

/*
===============
Portal_EntityFlood

The entity flood determines which areas are
"outside" on the map, which are then filled in.
Flowing from side s to side !s
===============
*/
static bool Portal_EntityFlood(const portal_t *p, int32_t s)
{
    if (p->nodes[0]->planenum != PLANENUM_LEAF || p->nodes[1]->planenum != PLANENUM_LEAF)
        Error("Portal_EntityFlood: not a leaf");

    // can never cross to a solid
    if ((p->nodes[0]->contents.native & CONTENTS_SOLID) || (p->nodes[1]->contents.native & CONTENTS_SOLID))
        return false;

    // can flood through everything else
    return true;
}

/*
=============
FloodAreas_r
=============
*/
static void FloodAreas_r(mapentity_t *entity, node_t *node)
{
    if (node->contents.native == Q2_CONTENTS_AREAPORTAL) {
        // this node is part of an area portal;
        // if the current area has allready touched this
        // portal, we are done
        if (entity->portalareas[0] == c_areas || entity->portalareas[1] == c_areas)
            return;

        // note the current area as bounding the portal
        if (entity->portalareas[1]) {
            // FIXME: entity #
            LogPrint("WARNING: areaportal entity touches > 2 areas\n  Node Bounds: {} -> {}\n", node->bounds.mins(),
                node->bounds.maxs());
            return;
        }

        if (entity->portalareas[0])
            entity->portalareas[1] = c_areas;
        else
            entity->portalareas[0] = c_areas;

        return;
    }

    if (node->area)
        return; // already got it

    node->area = c_areas;

    int32_t s;

    for (portal_t *p = node->portals; p; p = p->next[s]) {
        s = (p->nodes[1] == node);
#if 0
		if (p->nodes[!s]->occupied)
			continue;
#endif
        if (!Portal_EntityFlood(p, s))
            continue;

        FloodAreas_r(entity, p->nodes[!s]);
    }
}

/*
=============
FindAreas_r

Just decend the tree, and for each node that hasn't had an
area set, flood fill out from there
=============
*/
static void FindAreas_r(mapentity_t *entity, node_t *node)
{
    if (node->planenum != PLANENUM_LEAF) {
        FindAreas_r(entity, node->children[0]);
        FindAreas_r(entity, node->children[1]);
        return;
    }

    if (node->area)
        return; // already got it

    if (node->contents.native & Q2_CONTENTS_SOLID)
        return;

    // FIXME: how to do this since the nodes are destroyed by this point?
    // if (!node->occupied)
    //	return;			// not reachable by entities

    // area portals are always only flooded into, never
    // out of
    if (node->contents.native == Q2_CONTENTS_AREAPORTAL)
        return;

    c_areas++;
    FloodAreas_r(entity, node);
}

/*
=============
SetAreaPortalAreas_r

Just decend the tree, and for each node that hasn't had an
area set, flood fill out from there
=============
*/
static void SetAreaPortalAreas_r(mapentity_t *entity, node_t *node)
{
    if (node->planenum != PLANENUM_LEAF) {
        SetAreaPortalAreas_r(entity, node->children[0]);
        SetAreaPortalAreas_r(entity, node->children[1]);
        return;
    }

    if (node->contents.native != Q2_CONTENTS_AREAPORTAL)
        return;

    if (node->area)
        return; // already set

    node->area = entity->portalareas[0];
    if (!entity->portalareas[1]) {
        // FIXME: entity #
        LogPrint("WARNING: areaportal entity doesn't touch two areas\n  Node Bounds: {} -> {}\n",
            qv::to_string(entity->bounds.mins()), qv::to_string(entity->bounds.maxs()));
        return;
    }
}

/*
=============
FloodAreas

Mark each leaf with an area, bounded by CONTENTS_AREAPORTAL
=============
*/
static void FloodAreas(mapentity_t *entity, node_t *headnode)
{
    LogPrint(LOG_PROGRESS, "---- {} ----\n", __func__);
    FindAreas_r(entity, headnode);
    SetAreaPortalAreas_r(entity, headnode);
    LogPrint(LOG_STAT, "{:5} areas\n", c_areas);
}

/*
=============
EmitAreaPortals

=============
*/
static void EmitAreaPortals(node_t *headnode)
{
    LogPrint(LOG_PROGRESS, "---- {} ----\n", __func__);

    map.bsp.dareaportals.emplace_back();
    map.bsp.dareas.emplace_back();

    for (size_t i = 1; i <= c_areas; i++) {
        darea_t &area = map.bsp.dareas.emplace_back();
        area.firstareaportal = map.bsp.dareaportals.size();

        for (auto &e : map.entities) {

            if (!e.areaportalnum)
                continue;
            dareaportal_t &dp = map.bsp.dareaportals.emplace_back();

            if (e.portalareas[0] == i) {
                dp.portalnum = e.areaportalnum;
                dp.otherarea = e.portalareas[1];
            } else if (e.portalareas[1] == i) {
                dp.portalnum = e.areaportalnum;
                dp.otherarea = e.portalareas[0];
            }
        }

        area.numareaportals = map.bsp.dareaportals.size() - area.firstareaportal;
    }

    LogPrint(LOG_STAT, "{:5} numareas\n", map.bsp.dareas.size());
    LogPrint(LOG_STAT, "{:5} numareaportals\n", map.bsp.dareaportals.size());
}

/*
===============
ProcessEntity
===============
*/
static void ProcessEntity(mapentity_t *entity, const int hullnum)
{
    int i, firstface;
    surface_t *surfs;
    node_t *nodes;

    /* No map brushes means non-bmodel entity.
       We need to handle worldspawn containing no brushes, though. */
    if (!entity->nummapbrushes && entity != pWorldEnt())
        return;

    /*
     * func_group and func_detail entities get their brushes added to the
     * worldspawn
     */
    if (IsWorldBrushEntity(entity) || IsNonRemoveWorldBrushEntity(entity))
        return;

    // Export a blank model struct, and reserve the index (only do this once, for all hulls)
    if (!entity->outputmodelnumber.has_value()) {
        entity->outputmodelnumber = map.bsp.dmodels.size();
        map.bsp.dmodels.emplace_back();
    }

    if (entity != pWorldEnt()) {
        if (entity == pWorldEnt() + 1)
            LogPrint(LOG_PROGRESS, "---- Internal Entities ----\n");

        std::string mod = fmt::format("*{}", entity->outputmodelnumber.value());

        if (options.fVerbose)
            PrintEntity(entity);

        if (hullnum <= 0)
            LogPrint(LOG_STAT, "     MODEL: {}\n", mod);
        SetKeyValue(entity, "model", mod.c_str());
    }

    /*
     * Init the entity
     */
    entity->brushes = NULL;
    entity->solid = NULL;
    entity->sky = NULL;
    entity->detail = NULL;
    entity->detail_illusionary = NULL;
    entity->detail_fence = NULL;
    entity->liquid = NULL;
    entity->numbrushes = 0;
    entity->bounds = {};

    /*
     * Convert the map brushes (planes) into BSP brushes (polygons)
     */
    LogPrint(LOG_PROGRESS, "---- Brush_LoadEntity ----\n");
    Brush_LoadEntity(entity, entity, hullnum);

    // FIXME: copied and pasted to BSPX_CreateBrushList
    /*
     * If this is the world entity, find all func_group and func_detail
     * entities and add their brushes with the appropriate contents flag set.
     */
    if (entity == pWorldEnt()) {
        /*
         * We no longer care about the order of adding func_detail and func_group,
         * Entity_SortBrushes will sort the brushes
         */
        for (i = 1; i < map.numentities(); i++) {
            mapentity_t *source = &map.entities.at(i);

            /* Load external .map and change the classname, if needed */
            ProcessExternalMapEntity(source);

            ProcessAreaPortal(source);

            if (IsWorldBrushEntity(source) || IsNonRemoveWorldBrushEntity(source)) {
                Brush_LoadEntity(entity, source, hullnum);
            }
        }
    }

    /* Print brush counts */
    {
        int solidcount = Brush_ListCount(entity->solid);
        int skycount = Brush_ListCount(entity->sky);
        int detail_all_count = Brush_ListCount(entity->detail);
        int detail_illusionarycount = Brush_ListCount(entity->detail_illusionary);
        int detail_fence_count = Brush_ListCount(entity->detail_fence);
        int liquidcount = Brush_ListCount(entity->liquid);

        int nondetailcount = (solidcount + skycount + liquidcount);
        int detailcount = detail_all_count;

        LogPrint(LOG_STAT, "     {:8} brushes\n", nondetailcount);
        if (detailcount > 0) {
            LogPrint(LOG_STAT, "     {:8} detail\n", detailcount);
        }
        if (detail_fence_count > 0) {
            LogPrint(LOG_STAT, "     {:8} detail fence\n", detail_fence_count);
        }
        if (detail_illusionarycount > 0) {
            LogPrint(LOG_STAT, "     {:8} detail illusionary\n", detail_illusionarycount);
        }

        LogPrint(LOG_STAT, "     {:8} planes\n", map.numplanes());
    }

    Entity_SortBrushes(entity);

    if (!entity->brushes && hullnum) {
        PrintEntity(entity);
        FError("Entity with no valid brushes");
    }

    /*
     * Take the brush_t's and clip off all overlapping and contained faces,
     * leaving a perfect skin of the model with no hidden faces
     */
    surfs = CSGFaces(entity);

    if (options.fObjExport && entity == pWorldEnt() && hullnum <= 0) {
        ExportObj_Surfaces("post_csg", surfs);
    }

    if (hullnum > 0) {
        nodes = SolidBSP(entity, surfs, true);
        if (entity == pWorldEnt() && !options.fNofill) {
            // assume non-world bmodels are simple
            PortalizeWorld(entity, nodes, hullnum);
            if (FillOutside(nodes, hullnum)) {
                // Free portals before regenerating new nodes
                FreeAllPortals(nodes);
                surfs = GatherNodeFaces(nodes);
                // make a really good tree
                nodes = SolidBSP(entity, surfs, false);

                DetailToSolid(nodes);
            }
        }
        ExportClipNodes(entity, nodes, hullnum);
    } else {
        /*
         * SolidBSP generates a node tree
         *
         * if not the world, make a good tree first the world is just
         * going to make a bad tree because the outside filling will
         * force a regeneration later.
         *
         * Forcing the good tree for the first pass on the world can
         * sometimes result in reduced marksurfaces at the expense of
         * longer processing time.
         */
        if (options.forceGoodTree)
            nodes = SolidBSP(entity, surfs, false);
        else
            nodes = SolidBSP(entity, surfs, entity == pWorldEnt());

        // build all the portals in the bsp tree
        // some portals are solid polygons, and some are paths to other leafs
        if (entity == pWorldEnt() && !options.fNofill) {
            // assume non-world bmodels are simple
            PortalizeWorld(entity, nodes, hullnum);
            if (FillOutside(nodes, hullnum)) {
                FreeAllPortals(nodes);

                // get the remaining faces together into surfaces again
                surfs = GatherNodeFaces(nodes);

                // merge polygons
                MergeAll(surfs);

                // make a really good tree
                nodes = SolidBSP(entity, surfs, false);

                // convert detail leafs to solid
                DetailToSolid(nodes);

                // make the real portals for vis tracing
                PortalizeWorld(entity, nodes, hullnum);

                TJunc(entity, nodes);
            }

            // Area portals
            /*if (options.target_game->id == GAME_QUAKE_II) {
                FloodAreas(entity, nodes);
                EmitAreaPortals(nodes);
            }*/
            // TEMP
            if (options.target_game->id == GAME_QUAKE_II) {
                map.bsp.dareaportals.emplace_back();

                map.bsp.dareas.emplace_back();
                map.bsp.dareas.push_back({0, 1});
            }

            FreeAllPortals(nodes);
        }

        // bmodels
        if (entity != pWorldEnt()) {
            TJunc(entity, nodes);
        }

        // convert detail leafs to solid (in case we didn't make the call above)
        DetailToSolid(nodes);

        if (options.fObjExport && entity == pWorldEnt()) {
            ExportObj_Nodes("pre_makefaceedges_plane_faces", nodes);
            ExportObj_Marksurfaces("pre_makefaceedges_marksurfaces", nodes);
        }

        firstface = MakeFaceEdges(entity, nodes);

        if (options.target_game->id == GAME_QUAKE_II) {
            ExportBrushList(entity, nodes, brush_offset);
        }

        ExportDrawNodes(entity, nodes, firstface);
    }

    FreeBrushes(entity);
}

/*
=================
UpdateEntLump

=================
*/
static void UpdateEntLump(void)
{
    int modnum, i;
    char modname[10];
    mapentity_t *entity;

    LogPrint(LOG_STAT, "     Updating entities lump...\n");

    modnum = 1;
    for (i = 1; i < map.numentities(); i++) {
        entity = &map.entities.at(i);

        /* Special handling for misc_external_map.
           Duplicates some logic from ProcessExternalMapEntity. */
        bool is_misc_external_map = false;
        if (!Q_strcasecmp(ValueForKey(entity, "classname"), "misc_external_map")) {
            const char *new_classname = ValueForKey(entity, "_external_map_classname");

            SetKeyValue(entity, "classname", new_classname);
            SetKeyValue(entity, "origin", "0 0 0");

            /* Note: the classname could have switched to
             * a IsWorldBrushEntity entity (func_group, func_detail),
             * or a bmodel entity (func_wall
             */
            is_misc_external_map = true;
        }

        bool isBrushEnt = (entity->nummapbrushes > 0) || is_misc_external_map;
        if (!isBrushEnt)
            continue;

        if (IsWorldBrushEntity(entity) || IsNonRemoveWorldBrushEntity(entity))
            continue;

        snprintf(modname, sizeof(modname), "*%d", modnum);
        SetKeyValue(entity, "model", modname);
        modnum++;

        /* Do extra work for rotating entities if necessary */
        const char *classname = ValueForKey(entity, "classname");
        if (!strncmp(classname, "rotate_", 7))
            FixRotateOrigin(entity);
    }

    WriteEntitiesToString();
    UpdateBSPFileEntitiesLump();

    if (!options.fAllverbose) {
        options.fVerbose = false;
        log_mask &= ~((1 << LOG_STAT) | (1 << LOG_PROGRESS));
    }
}

/*
Actually writes out the final bspx BRUSHLIST lump
This lump replaces the clipnodes stuff for custom collision sizes.
*/
void BSPX_Brushes_Finalize(struct bspxbrushes_s *ctx)
{
    // Actually written in WriteBSPFile()
    map.exported_bspxbrushes = std::move(ctx->lumpdata);
}
void BSPX_Brushes_Init(struct bspxbrushes_s *ctx)
{
    ctx->lumpdata.clear();
}

static void vec_push_bytes(std::vector<uint8_t> &vec, const void *data, size_t count)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);

    vec.insert(vec.end(), bytes, bytes + count);
}

/*
WriteBrushes
Generates a submodel's direct brush information to a separate file, so the engine doesn't need to depend upon specific
hull sizes
*/

void BSPX_Brushes_AddModel(struct bspxbrushes_s *ctx, int modelnum, brush_t *brushes)
{
    brush_t *b;
    face_t *f;

    bspxbrushes_permodel permodel;
    permodel.numbrushes = 0;
    permodel.numfaces = 0;
    for (b = brushes; b; b = b->next) {
        permodel.numbrushes++;
        for (f = b->faces; f; f = f->next) {
            /*skip axial*/
            if (fabs(map.planes[f->planenum].normal[0]) == 1 || fabs(map.planes[f->planenum].normal[1]) == 1 ||
                fabs(map.planes[f->planenum].normal[2]) == 1)
                continue;
            permodel.numfaces++;
        }
    }

    permodel.ver = LittleLong(1);
    permodel.modelnum = LittleLong(modelnum);
    permodel.numbrushes = LittleLong(permodel.numbrushes);
    permodel.numfaces = LittleLong(permodel.numfaces);
    vec_push_bytes(ctx->lumpdata, &permodel, sizeof(permodel));

    for (b = brushes; b; b = b->next) {
        bspxbrushes_perbrush perbrush;
        perbrush.numfaces = 0;
        for (f = b->faces; f; f = f->next) {
            /*skip axial*/
            if (fabs(map.planes[f->planenum].normal[0]) == 1 || fabs(map.planes[f->planenum].normal[1]) == 1 ||
                fabs(map.planes[f->planenum].normal[2]) == 1)
                continue;
            perbrush.numfaces++;
        }

        perbrush.mins[0] = LittleFloat(b->bounds.mins()[0]);
        perbrush.mins[1] = LittleFloat(b->bounds.mins()[1]);
        perbrush.mins[2] = LittleFloat(b->bounds.mins()[2]);
        perbrush.maxs[0] = LittleFloat(b->bounds.maxs()[0]);
        perbrush.maxs[1] = LittleFloat(b->bounds.maxs()[1]);
        perbrush.maxs[2] = LittleFloat(b->bounds.maxs()[2]);
        switch (b->contents.native) {
            // contents should match the engine.
            case CONTENTS_EMPTY: // really an error, but whatever
            case CONTENTS_SOLID: // these are okay
            case CONTENTS_WATER:
            case CONTENTS_SLIME:
            case CONTENTS_LAVA:
            case CONTENTS_SKY:
                if (b->contents.extended & CFLAGS_CLIP) {
                    perbrush.contents = -8;
                } else {
                    perbrush.contents = b->contents.native;
                }
                break;
            //              case CONTENTS_LADDER:
            //                      perbrush.contents = -16;
            //                      break;
            default: {
                if (b->contents.is_clip()) {
                    perbrush.contents = -8;
                } else {
                    LogPrint("WARNING: Unknown contents: {}. Translating to solid.\n",
                        b->contents.to_string(options.target_game));
                    perbrush.contents = CONTENTS_SOLID;
                }
                break;
            }
        }
        perbrush.contents = LittleShort(perbrush.contents);
        perbrush.numfaces = LittleShort(perbrush.numfaces);
        vec_push_bytes(ctx->lumpdata, &perbrush, sizeof(perbrush));

        for (f = b->faces; f; f = f->next) {
            bspxbrushes_perface perface;
            /*skip axial*/
            if (fabs(map.planes[f->planenum].normal[0]) == 1 || fabs(map.planes[f->planenum].normal[1]) == 1 ||
                fabs(map.planes[f->planenum].normal[2]) == 1)
                continue;

            if (f->planeside) {
                perface.normal[0] = -map.planes[f->planenum].normal[0];
                perface.normal[1] = -map.planes[f->planenum].normal[1];
                perface.normal[2] = -map.planes[f->planenum].normal[2];
                perface.dist = -map.planes[f->planenum].dist;
            } else {
                perface.normal[0] = map.planes[f->planenum].normal[0];
                perface.normal[1] = map.planes[f->planenum].normal[1];
                perface.normal[2] = map.planes[f->planenum].normal[2];
                perface.dist = map.planes[f->planenum].dist;
            }

            vec_push_bytes(ctx->lumpdata, &perface, sizeof(perface));
        }
    }
}

/* for generating BRUSHLIST bspx lump */
static void BSPX_CreateBrushList(void)
{
    mapentity_t *ent;
    int entnum;
    int modelnum;
    const char *mod;
    struct bspxbrushes_s ctx;

    if (!options.fbspx_brushes)
        return;

    BSPX_Brushes_Init(&ctx);

    for (entnum = 0; entnum < map.numentities(); entnum++) {
        ent = &map.entities.at(entnum);
        if (ent == pWorldEnt())
            modelnum = 0;
        else {
            mod = ValueForKey(ent, "model");
            if (*mod != '*')
                continue;
            modelnum = atoi(mod + 1);
        }

        ent->brushes = NULL;
        ent->detail_illusionary = NULL;
        ent->liquid = NULL;
        ent->detail_fence = NULL;
        ent->detail = NULL;
        ent->sky = NULL;
        ent->solid = NULL;

        ent->numbrushes = 0;
        Brush_LoadEntity(ent, ent, -1);

        // FIXME: copied and pasted from ProcessEntity
        /*
         * If this is the world entity, find all func_group and func_detail
         * entities and add their brushes with the appropriate contents flag set.
         */
        if (ent == pWorldEnt()) {
            /*
             * We no longer care about the order of adding func_detail and func_group,
             * Entity_SortBrushes will sort the brushes
             */
            for (int i = 1; i < map.numentities(); i++) {
                mapentity_t *source = &map.entities.at(i);

                /* Load external .map and change the classname, if needed */
                ProcessExternalMapEntity(source);

                if (IsWorldBrushEntity(source) || IsNonRemoveWorldBrushEntity(source)) {
                    Brush_LoadEntity(ent, source, -1);
                }
            }
        }

        Entity_SortBrushes(ent);

        if (!ent->brushes)
            continue; // non-bmodel entity

        BSPX_Brushes_AddModel(&ctx, modelnum, ent->brushes);
        FreeBrushes(ent);
    }

    BSPX_Brushes_Finalize(&ctx);
}

/*
=================
CreateSingleHull

=================
*/
static void CreateSingleHull(const int hullnum)
{
    int i;
    mapentity_t *entity;

    LogPrint("Processing hull {}...\n", hullnum);

    // for each entity in the map file that has geometry
    for (i = 0; i < map.numentities(); i++) {
        entity = &map.entities.at(i);
        ProcessEntity(entity, hullnum);
        if (!options.fAllverbose) {
            options.fVerbose = false; // don't print rest of entities
            log_mask &= ~((1 << LOG_STAT) | (1 << LOG_PROGRESS));
        }
    }
}

/*
=================
CreateHulls

=================
*/
static void CreateHulls(void)
{
    /* create the hulls sequentially */
    if (!options.fNoverbose) {
        options.fVerbose = true;
        log_mask |= (1 << LOG_STAT) | (1 << LOG_PROGRESS);
    }

    auto &hulls = options.target_game->get_hull_sizes();

    // game has no hulls, so we have to export brush lists and stuff.
    if (!hulls.size()) {
        CreateSingleHull(-1);
        return;
    }

    // we got hulls!
    for (size_t i = 0; i < hulls.size(); i++) {
        /* ignore the clipping hulls altogether */
        if (i && options.fNoclip) {
            return;
        }

        CreateSingleHull(i);
    }
}

static bool wadlist_tried_loading = false;

void EnsureTexturesLoaded()
{
    if (wadlist_tried_loading)
        return;

    wadlist_tried_loading = true;

    const char *wadstring = ValueForKey(pWorldEnt(), "_wad");
    if (!wadstring[0])
        wadstring = ValueForKey(pWorldEnt(), "wad");
    if (!wadstring[0])
        LogPrint("WARNING: No wad or _wad key exists in the worldmodel\n");
    else
        WADList_Init(wadstring);

    if (!wadlist.size()) {
        if (wadstring[0])
            LogPrint("WARNING: No valid WAD filenames in worldmodel\n");

        /* Try the default wad name */
        std::filesystem::path defaultwad = options.szMapName;
        defaultwad.replace_extension("wad");

        WADList_Init(defaultwad.string().c_str());

        if (wadlist.size())
            LogPrint("Using default WAD: {}\n", defaultwad);
    }
}

/*
=================
ProcessFile
=================
*/
static void ProcessFile(void)
{
    // load brushes and entities
    SetQdirFromPath(options.target_game->base_dir, options.szMapName);
    LoadMapFile();
    if (options.fConvertMapFormat) {
        ConvertMapFile();
        return;
    }
    if (options.fOnlyents) {
        UpdateEntLump();
        return;
    }

    // this can happen earlier if brush primitives are in use, because we need texture sizes then
    EnsureTexturesLoaded();

    // init the tables to be shared by all models
    BeginBSPFile();

    if (!options.fAllverbose) {
        options.fVerbose = false;
        log_mask &= ~((1 << LOG_STAT) | (1 << LOG_PROGRESS));
    }
    CreateHulls();

    WriteEntitiesToString();
    WADList_Process();
    BSPX_CreateBrushList();
    FinishBSPFile();

    wadlist.clear();
}

/*
==============
PrintOptions
==============
*/
[[noreturn]] static void PrintOptions()
{
    printf(
        "\n"
        "qbsp performs geometric level processing of Quake .MAP files to create\n"
        "Quake .BSP files.\n\n"
        "qbsp [options] sourcefile [destfile]\n\n"
        "Options:\n"
        "   -nofill         Doesn't perform outside filling\n"
        "   -noclip         Doesn't build clip hulls\n"
        "   -noskip         Doesn't remove faces with the 'skip' texture\n"
        "   -nodetail       Convert func_detail to structural\n"
        "   -onlyents       Only updates .MAP entities\n"
        "   -verbose        Print out more .MAP information\n"
        "   -noverbose      Print out almost no information at all\n"
        "   -splitspecial   Doesn't combine sky and water faces into one large face\n"
        "   -splitsky       Doesn't combine sky faces into one large face\n"
        "   -splitturb      Doesn't combine water faces into one large face\n"
        "   -notranswater   Computes portal information for opaque water\n"
        "   -transsky       Computes portal information for transparent sky\n"
        "   -notex          Write only placeholder textures, to depend upon replacements, to keep file sizes down, or to skirt copyrights\n"
        "   -nooldaxis      Uses alternate texture alignment which was default in tyrutils-ericw v0.15.1 and older\n"
        "   -forcegoodtree  Force use of expensive processing for SolidBSP stage\n"
        "   -nopercent      Prevents output of percent completion information\n"
        "   -wrbrushes      (bspx) Includes a list of brushes for brush-based collision\n"
        "   -wrbrushesonly  -wrbrushes with -noclip\n"
        "   -hexen2         Generate a BSP compatible with hexen2 engines\n"
        "   -hlbsp          Request output in Half-Life bsp format\n"
        "   -bsp2           Request output in bsp2 format\n"
        "   -2psb           Request output in 2psb format (RMQ compatible)\n"
        "   -leakdist  [n]  Space between leakfile points (default 2)\n"
        "   -subdivide [n]  Use different texture subdivision (default 240)\n"
        "   -wadpath <dir>  Search this directory for wad files (mips will be embedded unless -notex)\n"
        "   -xwadpath <dir> Search this directory for wad files (mips will NOT be embedded, avoiding texture license issues)\n"
        "   -oldrottex      Use old rotate_ brush texturing aligned at (0 0 0)\n"
        "   -maxnodesize [n]Triggers simpler BSP Splitting when node exceeds size (default 1024, 0 to disable)\n"
        "   -epsilon [n]    Customize ON_EPSILON (default 0.0001)\n"
        "   -forceprt1      Create a PRT1 file for loading in editors, even if PRT2 is required to run vis.\n"
        "   -objexport      Export the map file as an .OBJ model after the CSG phase\n"
        "   -omitdetail     func_detail brushes are omitted from the compile\n"
        "   -omitdetailwall          func_detail_wall brushes are omitted from the compile\n"
        "   -omitdetailillusionary   func_detail_illusionary brushes are omitted from the compile\n"
        "   -omitdetailfence         func_detail_fence brushes are omitted from the compile\n"
        "   -convert <fmt>  Convert a .MAP to a different .MAP format. fmt can be: quake, quake2, valve, bp (brush primitives).\n"
        "   -expand         Write hull 1 expanded brushes to expanded.map for debugging\n"
        "   -leaktest       Make compilation fail if the map leaks\n"
        "   -contenthack    Hack to fix leaks through solids. Causes missing faces in some cases so disabled by default.\n"
        "   -nothreads      Disable multithreading\n"
        "   sourcefile      .MAP file to process\n"
        "   destfile        .BSP file to output\n");

    exit(1);
}

/*
=============
GetTok

Gets tokens from command line string.
=============
*/
static char *GetTok(char *szBuf, char *szEnd)
{
    char *szTok;

    if (szBuf >= szEnd)
        return NULL;

    // Eliminate leading whitespace
    while (*szBuf == ' ' || *szBuf == '\n' || *szBuf == '\t' || *szBuf == '\r')
        szBuf++;

    if (szBuf >= szEnd)
        return NULL;

    // Three cases: strings, options, and none-of-the-above.
    if (*szBuf == '\"') {
        szBuf++;
        szTok = szBuf;
        while (*szBuf != 0 && *szBuf != '\"' && *szBuf != '\n' && *szBuf != '\r')
            szBuf++;
    } else if (*szBuf == '-' || *szBuf == '/') {
        szTok = szBuf;
        while (*szBuf != ' ' && *szBuf != '\n' && *szBuf != '\t' && *szBuf != '\r' && *szBuf != 0)
            szBuf++;
    } else {
        szTok = szBuf;
        while (*szBuf != ' ' && *szBuf != '\n' && *szBuf != '\t' && *szBuf != '\r' && *szBuf != 0)
            szBuf++;
    }

    if (*szBuf != 0)
        *szBuf = 0;
    return szTok;
}

/*
==================
ParseOptions
==================
*/
static void ParseOptions(char *szOptions)
{
    char *szTok, *szTok2;
    char *szEnd;
    int NameCount = 0;

    // temporary flags
    bool hexen2 = false;

    szEnd = szOptions + strlen(szOptions);
    szTok = GetTok(szOptions, szEnd);
    while (szTok) {
        if (szTok[0] != '-') {
            /* Treat as filename */
            if (NameCount == 0)
                options.szMapName = szTok;
            else if (NameCount == 1)
                options.szBSPName = szTok;
            else
                FError("Unknown option '{}'", szTok);
            NameCount++;
        } else {
            szTok++;
            if (!Q_strcasecmp(szTok, "nofill"))
                options.fNofill = true;
            else if (!Q_strcasecmp(szTok, "noclip"))
                options.fNoclip = true;
            else if (!Q_strcasecmp(szTok, "noskip"))
                options.fNoskip = true;
            else if (!Q_strcasecmp(szTok, "nodetail"))
                options.fNodetail = true;
            else if (!Q_strcasecmp(szTok, "onlyents"))
                options.fOnlyents = true;
            else if (!Q_strcasecmp(szTok, "verbose")) {
                options.fAllverbose = true;
                log_mask |= 1 << LOG_VERBOSE;
            } else if (!Q_strcasecmp(szTok, "splitspecial"))
                options.fSplitspecial = true;
            else if (!Q_strcasecmp(szTok, "splitsky"))
                options.fSplitsky = true;
            else if (!Q_strcasecmp(szTok, "splitturb"))
                options.fSplitturb = true;
            else if (!Q_strcasecmp(szTok, "notranswater"))
                options.fTranswater = false;
            else if (!Q_strcasecmp(szTok, "transwater"))
                options.fTranswater = true;
            else if (!Q_strcasecmp(szTok, "transsky"))
                options.fTranssky = true;
            else if (!Q_strcasecmp(szTok, "notex"))
                options.fNoTextures = true;
            else if (!Q_strcasecmp(szTok, "oldaxis"))
                LogPrint(
                    "-oldaxis is now the default and the flag is ignored.\nUse -nooldaxis to get the alternate behaviour.\n");
            else if (!Q_strcasecmp(szTok, "nooldaxis"))
                options.fOldaxis = false;
            else if (!Q_strcasecmp(szTok, "forcegoodtree"))
                options.forceGoodTree = true;
            else if (!Q_strcasecmp(szTok, "noverbose")) {
                options.fNoverbose = true;
                log_mask &= ~((1 << LOG_PERCENT) | (1 << LOG_STAT) | (1 << LOG_PROGRESS));
            } else if (!Q_strcasecmp(szTok, "nopercent")) {
                options.fNopercent = true;
                log_mask &= ~(1 << LOG_PERCENT);
            } else if (!Q_strcasecmp(szTok, "hexen2"))
                hexen2 = true; // can be combined with -bsp2 or -2psb
            else if (!Q_strcasecmp(szTok, "q2bsp"))
                options.target_version = &bspver_q2;
            else if (!Q_strcasecmp(szTok, "qbism"))
                options.target_version = &bspver_qbism;
            else if (!Q_strcasecmp(szTok, "wrbrushes") || !Q_strcasecmp(szTok, "bspx"))
                options.fbspx_brushes = true;
            else if (!Q_strcasecmp(szTok, "wrbrushesonly") || !Q_strcasecmp(szTok, "bspxonly")) {
                options.fbspx_brushes = true;
                options.fNoclip = true;
            } else if (!Q_strcasecmp(szTok, "hlbsp")) {
                options.target_version = &bspver_hl;
            } else if (!Q_strcasecmp(szTok, "bsp2")) {
                options.target_version = &bspver_bsp2;
            } else if (!Q_strcasecmp(szTok, "2psb")) {
                options.target_version = &bspver_bsp2rmq;
            } else if (!Q_strcasecmp(szTok, "leakdist")) {
                szTok2 = GetTok(szTok + strlen(szTok) + 1, szEnd);
                if (!szTok2)
                    FError("Invalid argument to option {}", szTok);
                options.dxLeakDist = atoi(szTok2);
                szTok = szTok2;
            } else if (!Q_strcasecmp(szTok, "subdivide")) {
                szTok2 = GetTok(szTok + strlen(szTok) + 1, szEnd);
                if (!szTok2)
                    FError("Invalid argument to option {}", szTok);
                options.dxSubdivide = atoi(szTok2);
                szTok = szTok2;
            } else if (!Q_strcasecmp(szTok, "wadpath") || !Q_strcasecmp(szTok, "xwadpath")) {
                szTok2 = GetTok(szTok + strlen(szTok) + 1, szEnd);
                if (!szTok2)
                    FError("Invalid argument to option {}", szTok);

                std::string wadpath = szTok2;
                /* Remove trailing /, if any */
                if (wadpath.size() > 0 && wadpath[wadpath.size() - 1] == '/') {
                    wadpath.resize(wadpath.size() - 1);
                }

                options_t::wadpath wp;
                wp.external = !!Q_strcasecmp(szTok, "wadpath");
                wp.path = wadpath;
                options.wadPathsVec.push_back(wp);

                szTok = szTok2;
            } else if (!Q_strcasecmp(szTok, "oldrottex")) {
                options.fixRotateObjTexture = false;
            } else if (!Q_strcasecmp(szTok, "maxnodesize")) {
                szTok2 = GetTok(szTok + strlen(szTok) + 1, szEnd);
                if (!szTok2)
                    FError("Invalid argument to option {}", szTok);
                options.maxNodeSize = atoi(szTok2);
                szTok = szTok2;
            } else if (!Q_strcasecmp(szTok, "midsplitsurffraction")) {
                szTok2 = GetTok(szTok + strlen(szTok) + 1, szEnd);
                if (!szTok2)
                    FError("Invalid argument to option {}", szTok);
                options.midsplitSurfFraction = qclamp((float)atof(szTok2), 0.0f, 1.0f);
                LogPrint("Switching to midsplit when node contains more than fraction {} of model's surfaces\n",
                    options.midsplitSurfFraction);

                szTok = szTok2;
            } else if (!Q_strcasecmp(szTok, "epsilon")) {
                szTok2 = GetTok(szTok + strlen(szTok) + 1, szEnd);
                if (!szTok2)
                    FError("Invalid argument to option {}", szTok);
                options.on_epsilon = atof(szTok2);
                szTok = szTok2;
            } else if (!Q_strcasecmp(szTok, "worldextent")) {
                szTok2 = GetTok(szTok + strlen(szTok) + 1, szEnd);
                if (!szTok2)
                    FError("Invalid argument to option {}", szTok);
                options.worldExtent = atof(szTok2);
                LogPrint("Overriding maximum world extents to +/- {} units\n", options.worldExtent);
                szTok = szTok2;
            } else if (!Q_strcasecmp(szTok, "objexport")) {
                options.fObjExport = true;
            } else if (!Q_strcasecmp(szTok, "omitdetail")) {
                options.fOmitDetail = true;
            } else if (!Q_strcasecmp(szTok, "omitdetailwall")) {
                options.fOmitDetailWall = true;
            } else if (!Q_strcasecmp(szTok, "omitdetailillusionary")) {
                options.fOmitDetailIllusionary = true;
            } else if (!Q_strcasecmp(szTok, "omitdetailfence")) {
                options.fOmitDetailFence = true;
            } else if (!Q_strcasecmp(szTok, "convert")) {
                szTok2 = GetTok(szTok + strlen(szTok) + 1, szEnd);
                if (!szTok2)
                    FError("Invalid argument to option {}", szTok);

                if (!Q_strcasecmp(szTok2, "quake")) {
                    options.convertMapFormat = conversion_t::quake;
                } else if (!Q_strcasecmp(szTok2, "quake2")) {
                    options.convertMapFormat = conversion_t::quake2;
                } else if (!Q_strcasecmp(szTok2, "valve")) {
                    options.convertMapFormat = conversion_t::valve;
                } else if (!Q_strcasecmp(szTok2, "bp")) {
                    options.convertMapFormat = conversion_t::bp;
                } else {
                    FError("'-convert' requires one of: quake,quake2,valve,bp");
                }

                options.fConvertMapFormat = true;
                szTok = szTok2;
            } else if (!Q_strcasecmp(szTok, "forceprt1")) {
                options.fForcePRT1 = true;
                LogPrint("WARNING: Forcing creation of PRT1.\n");
                LogPrint("         Only use this for viewing portals in a map editor.\n");
            } else if (!Q_strcasecmp(szTok, "expand")) {
                options.fTestExpand = true;
            } else if (!Q_strcasecmp(szTok, "leaktest")) {
                options.fLeakTest = true;
            } else if (!Q_strcasecmp(szTok, "contenthack")) {
                options.fContentHack = true;
            } else if (!Q_strcasecmp(szTok, "nothreads")) {
                options.fNoThreads = true;
            } else if (!Q_strcasecmp(szTok, "?") || !Q_strcasecmp(szTok, "help")) {
                PrintOptions();
            } else {
                FError("Unknown option '{}'", szTok);
            }
        }
        szTok = GetTok(szTok + strlen(szTok) + 1, szEnd);
    }

    // if we wanted hexen2, update it now
    if (hexen2) {
        if (options.target_version == &bspver_bsp2) {
            options.target_version = &bspver_h2bsp2;
        } else if (options.target_version == &bspver_bsp2rmq) {
            options.target_version = &bspver_h2bsp2rmq;
        } else {
            options.target_version = &bspver_h2;
        }
    }

    // force specific flags for Q2
    if (options.target_game->id == GAME_QUAKE_II) {
        options.fNoclip = true;
    }

    // update target game
    options.target_game = options.target_version->game;
}

/*
==================
InitQBSP
==================
*/
static void InitQBSP(int argc, const char **argv)
{
    int i;
    char *szBuf;
    int length;

    length = LoadFile("qbsp.ini", &szBuf, false);
    if (length) {
        LogPrint("Loading options from qbsp.ini\n");
        ParseOptions(szBuf);

        delete[] szBuf;
    }

    // Concatenate command line args
    length = 1;
    for (i = 1; i < argc; i++) {
        length += strlen(argv[i]) + 1;
        if (argv[i][0] != '-')
            length += 2; /* quotes */
    }
    szBuf = new char[length]{};
    for (i = 1; i < argc; i++) {
        /* Quote filenames for the parsing function */
        if (argv[i][0] != '-')
            strcat(szBuf, "\"");
        strcat(szBuf, argv[i]);
        if (argv[i][0] != '-')
            strcat(szBuf, "\" ");
        else
            strcat(szBuf, " ");
    }
    szBuf[length - 1] = 0;
    ParseOptions(szBuf);
    delete[] szBuf;

    if (options.szMapName.empty())
        PrintOptions();

    options.szMapName.replace_extension("map");

    // The .map extension gets removed right away anyways...
    if (options.szBSPName.empty())
        options.szBSPName = options.szMapName;

    /* Start logging to <bspname>.log */
    options.szBSPName.replace_extension("log");
    InitLog(options.szBSPName);

    LogPrintSilent(IntroString);

    /* If no wadpath given, default to the map directory */
    if (options.wadPathsVec.empty()) {
        options_t::wadpath wp;
        wp.external = false;
        wp.path = options.szMapName.parent_path();

        // If options.szMapName is a relative path, StrippedFilename will return the empty string.
        // In that case, don't add it as a wad path.
        if (!wp.path.empty()) {
            options.wadPathsVec.push_back(wp);
        }
    }

    // Remove already existing files
    if (!options.fOnlyents && !options.fConvertMapFormat) {
        options.szBSPName.replace_extension("bsp");
        remove(options.szBSPName);

        // Probably not the best place to do this
        LogPrint("Input file: {}\n", options.szMapName);
        LogPrint("Output file: {}\n\n", options.szBSPName);

        options.szBSPName.replace_extension("prt");
        remove(options.szBSPName);

        options.szBSPName.replace_extension("pts");
        remove(options.szBSPName);

        options.szBSPName.replace_extension("por");
        remove(options.szBSPName);
    }
}

#include <fstream>

/*
==================
main
==================
*/
int qbsp_main(int argc, const char **argv)
{
    LogPrint(IntroString);

    InitQBSP(argc, argv);

    // disable TBB if requested
    auto tbbOptions = std::unique_ptr<tbb::global_control>();
    if (options.fNoThreads) {
        tbbOptions = std::make_unique<tbb::global_control>(tbb::global_control::max_allowed_parallelism, 1);
    }

    // do it!
    auto start = I_FloatTime();
    ProcessFile();
    auto end = I_FloatTime();

    LogPrint("\n{:.3} seconds elapsed\n", (end - start));

    //      FreeAllMem();
    //      PrintMem();

    CloseLog();

    return 0;
}
