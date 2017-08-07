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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 by the Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Daniel Dunbar
 *                 Ton Roosendaal,
 *                 Ben Batt,
 *                 Brecht Van Lommel,
 *                 Campbell Barton
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 */

/** \file blender/modifiers/intern/MOD_displace.c
 *  \ingroup modifiers
 */

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "DNA_object_types.h"
#include "DNA_hair_types.h"

#include "BKE_cdderivedmesh.h"
#include "BKE_editstrands.h"
#include "BKE_hair.h"
#include "BKE_library.h"
#include "BKE_library_query.h"
#include "BKE_modifier.h"

#include "DEG_depsgraph_build.h"

#include "MOD_util.h"


static void initData(ModifierData *md)
{
	HairModifierData *hmd = (HairModifierData *) md;
	
	hmd->hair = BKE_hair_new();
	
	hmd->flag |= 0;
	
	hmd->edit = NULL;
}

static void copyData(ModifierData *md, ModifierData *target)
{
	HairModifierData *hmd = (HairModifierData *) md;
	HairModifierData *thmd = (HairModifierData *) target;

	if (thmd->hair) {
		BKE_hair_free(thmd->hair);
	}

	modifier_copyData_generic(md, target);
	
	if (hmd->hair) {
		thmd->hair = BKE_hair_copy(hmd->hair);
	}
	
	thmd->edit = NULL;
}

static void freeData(ModifierData *md)
{
	HairModifierData *hmd = (HairModifierData *) md;
	
	if (hmd->hair) {
		BKE_hair_free(hmd->hair);
	}
	
	if (hmd->edit) {
		BKE_editstrands_free(hmd->edit);
		MEM_freeN(hmd->edit);
	}
}

static DerivedMesh *applyModifier(ModifierData *md, Object *UNUSED(ob),
                                  DerivedMesh *dm,
                                  ModifierApplyFlag UNUSED(flag))
{
	HairModifierData *hmd = (HairModifierData *) md;
	
	UNUSED_VARS(hmd);
	
	return dm;
}

ModifierTypeInfo modifierType_Hair = {
	/* name */              "Hair",
	/* structName */        "HairModifierData",
	/* structSize */        sizeof(HairModifierData),
	/* type */              eModifierTypeType_NonGeometrical,
	/* flags */             eModifierTypeFlag_AcceptsMesh |
	                        eModifierTypeFlag_SupportsEditmode,

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
	/* isDisabled */        NULL,
	/* updateDepgraph */    NULL,
	/* updateDepsgraph */   NULL,
	/* dependsOnTime */     NULL,
	/* dependsOnNormals */	NULL,
	/* foreachObjectLink */ NULL,
	/* foreachIDLink */     NULL,
	/* foreachTexLink */    NULL,
};
