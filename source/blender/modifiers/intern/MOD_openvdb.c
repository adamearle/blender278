/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s):
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/modifiers/intern/MOD_openvdb.c
 *  \ingroup modifiers
 */

#include <stdio.h>

#include "DNA_scene_types.h"
#include "DNA_object_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_smoke_types.h"

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"
#include "BLI_string.h"
#include "BLI_path_util.h"
#include "BLI_math.h"

#include "BKE_DerivedMesh.h"
#include "BKE_scene.h"
#include "BKE_global.h"
#include "BKE_mesh.h"
#include "BKE_main.h"
#include "BKE_smoke.h"
#include "BKE_pointcache.h"

#include "MEM_guardedalloc.h"

#include "MOD_modifiertypes.h"

#include "openvdb_capi.h"

static void initData(ModifierData *md)
{
	OpenVDBModifierData *vdbmd = (OpenVDBModifierData *)md;
	SmokeModifierData *smd = (SmokeModifierData *)modifier_new(eModifierType_Smoke);

	smd->type = MOD_SMOKE_TYPE_DOMAIN;

	smokeModifier_createType(smd);

	smd->domain->cache_file_format = PTCACHE_FILE_OPENVDB_EXTERN;
	smd->domain->vdb = vdbmd;
	smd->domain->clipping = 0.0f;

	vdbmd->smoke = smd;
	vdbmd->grids = NULL;
	vdbmd->numgrids = 0;

	vdbmd->up_axis = MOD_OVDB_AXIS_Z;
	vdbmd->front_axis = MOD_OVDB_AXIS_MIN_Y;

	vdbmd->frame_last = -1;

	vdbmd->flame_thickness = 1.0f;
}

static void freeData(ModifierData *md)
{
	OpenVDBModifierData *vdbmd = (OpenVDBModifierData *)md;
	SmokeModifierData *smd = vdbmd->smoke;

	MEM_SAFE_FREE(vdbmd->grids);

	modifier_free((ModifierData *)smd);
}

static void copyData(ModifierData *md, ModifierData *target)
{
	OpenVDBModifierData *vdbmd = (OpenVDBModifierData *)md;
	OpenVDBModifierData *tvdbmd = (OpenVDBModifierData *)target;
	PointCache *cache, *tcache;

	modifier_copyData_generic(md, target);

	tvdbmd->smoke = (SmokeModifierData *)modifier_new(eModifierType_Smoke);

	modifier_copyData((ModifierData *)vdbmd->smoke, (ModifierData *)tvdbmd->smoke);
	vdbmd->smoke->domain->vdb = vdbmd;

	cache = vdbmd->smoke->domain->point_cache[0];
	tcache = tvdbmd->smoke->domain->point_cache[0];

	tcache->startframe = cache->startframe;
	tcache->endframe = cache->endframe;

	tvdbmd->grids = MEM_dupallocN(vdbmd->grids);

	vdbmd->frame_last = -1;
}

static bool dependsOnTime(ModifierData *UNUSED(md))
{
	return true;
}

static bool isDisabled(ModifierData *md, int UNUSED(useRenderParams))
{
	OpenVDBModifierData *vdbmd = (OpenVDBModifierData *) md;

	/* leave it up to the modifier to check the file is valid on calculation */
	return (vdbmd->filepath[0] == '\0') || (vdbmd->up_axis % 3 == vdbmd->front_axis % 3);
}

static DerivedMesh *applyModifier(ModifierData *md, Object *ob,
                                  DerivedMesh *dm,
                                  ModifierApplyFlag flag)
{
#ifdef WITH_OPENVDB
	OpenVDBModifierData *vdbmd = (OpenVDBModifierData*) md;
	SmokeModifierData *smd = vdbmd->smoke;
	DerivedMesh *r_dm;
	char filepath[1024];
	int vdbflags = vdbmd->flags;
	short vdbsimplify = vdbmd->simplify;

	((ModifierData *)smd)->scene = md->scene;
	smd->domain->vdb = vdbmd;

	ob->dt = OB_WIRE;

	MEM_SAFE_FREE(vdbmd->grids);
	vdbmd->numgrids = 0;

	BLI_strncpy(filepath, vdbmd->filepath, sizeof(filepath));
	BLI_path_abs(filepath, ID_BLEND_PATH(G.main, (ID *)ob));

	if (BLI_exists(filepath)) {
		struct OpenVDBReader *reader = OpenVDBReader_create();
		OpenVDBReader_open(reader, filepath);

		vdbmd->numgrids = OpenVDB_get_num_grids(reader);

		vdbmd->grids = MEM_callocN(sizeof(*vdbmd->grids) * vdbmd->numgrids, "OpenVDB grid list");

		OpenVDB_fill_name_array(reader, (char **)vdbmd->grids);

		OpenVDBReader_free(reader);
	}

	invert_m4_m4(smd->domain->imat, ob->obmat);
	copy_m4_m4(smd->domain->obmat, ob->obmat);

	/* XXX Hack to avoid passing stuff all over the place. */
	if (flag & MOD_APPLY_RENDER) {
		if (vdbmd->flags & MOD_OPENVDB_SPARSE_RENDER) {
			vdbmd->flags |= MOD_OPENVDB_HIDE_VOLUME;
		}
		else {
			vdbmd->flags &= ~MOD_OPENVDB_HIDE_VOLUME;
		}

		if (!(vdbmd->flags & MOD_OPENVDB_SIMPLIFY_RENDER)) {
			vdbmd->simplify = 0;
		}

		if (!(vdbmd->flags & MOD_OPENVDB_IS_RENDER)) {
			vdbmd->frame_last = -1;
			vdbflags |= MOD_OPENVDB_IS_RENDER;
		}
	}
	else {
		if (G.is_rendering) {
			vdbmd->flags |= MOD_OPENVDB_NOW_RENDERING;
		}

		if ((vdbmd->flags & MOD_OPENVDB_HIDE_UNSELECTED) &&
			!(ob->flag & SELECT))
		{
			vdbmd->flags |= MOD_OPENVDB_HIDE_VOLUME;
		}

		if (vdbmd->flags & MOD_OPENVDB_IS_RENDER) {
			vdbmd->frame_last = -1;
			vdbflags &= ~MOD_OPENVDB_IS_RENDER;
		}
	}

	smd->domain->flags |= MOD_SMOKE_ADAPTIVE_DOMAIN;

	r_dm = modwrap_applyModifier((ModifierData *)smd, ob, dm, flag);

	smd->domain->flags &= ~MOD_SMOKE_ADAPTIVE_DOMAIN;

	vdbmd->flags = vdbflags;
	vdbmd->simplify = vdbsimplify;

	return r_dm;
#else
	UNUSED_VARS(md, ob, flag);
	return dm;
#endif
}


ModifierTypeInfo modifierType_OpenVDB = {
	/* name */              "OpenVDB",
	/* structName */        "OpenVDBModifierData",
	/* structSize */        sizeof(OpenVDBModifierData),
	/* type */              eModifierTypeType_Constructive,
    /* flags */             eModifierTypeFlag_AcceptsMesh |
	                        eModifierTypeFlag_UsesPointCache |
	                        eModifierTypeFlag_Single,

	/* copyData */          copyData,
	/* deformVerts */       NULL,
	/* deformMatrices */    NULL,
	/* deformVertsEM */     NULL,
	/* deformMatricesEM */  NULL,
	/* applyModifier */     applyModifier,
	/* applyModifierEM */   NULL,
	/* initData */          initData,
	/* requiredDataMask */  NULL,
	/* freeData */          freeData,
	/* isDisabled */        isDisabled,
	/* updateDepgraph */    NULL,
	/* updateDepsgraph */   NULL,
	/* dependsOnTime */     dependsOnTime,
	/* dependsOnNormals */  NULL,
	/* foreachObjectLink */ NULL,
	/* foreachIDLink */     NULL,
	/* foreachTexLink */    NULL,
};
