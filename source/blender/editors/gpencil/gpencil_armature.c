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
 * The Original Code is Copyright (C) 2018, Blender Foundation
 * This is a new part of Blender
 *
 * Contributor(s): Antonio Vazquez
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 * Operators for dealing with armatures and GP datablocks
 */

/** \file blender/editors/gpencil/gpencil_armature.c
 *  \ingroup edgpencil
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"
#include "BLI_ghash.h"
#include "BLI_math.h"
#include "BLI_string_utils.h"

#include "BLT_translation.h"

#include "DNA_armature_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.h"
#include "BKE_armature.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_object_deform.h"
#include "BKE_report.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "ED_gpencil.h"
#include "ED_mesh.h"

#include "DEG_depsgraph.h"

#include "gpencil_intern.h"

enum {
	GP_ARMATURE_NAME = 0,
	GP_ARMATURE_AUTO = 1
};

/* test if a point is inside cylinder
 * Return:  -1.0 if point is outside the cylinder
 *          o distance squared from cylinder axis if point is inside.
 */
static float test_point_in_cylynder(float pt1[3], float pt2[3],
									float lengthsq, float radius_sq, bGPDspoint *pt)
{
	float dx[3];	/* vector from line segment point 1 to point 2 */
	float pdx[3];	/* vector pd from point 1 to test point */
	float dot, dsq;

	sub_v3_v3v3(dx, pt2, pt1);
	sub_v3_v3v3(pdx, &pt->x, pt1);

	/* Dot the d and pd vectors to see if point lies behind the */
	dot = dot_v3v3(pdx, dx);

	/* If dot is less than zero the point is behind the pt1 cap.
	 * If greater than the cylinder axis line segment length squared
	 * then the point is outside the other end cap at pt2.
	 */
	if (dot < 0.0f || dot > lengthsq)
	{
		return(-1.0f);
	}
	else
	{
		/* Point lies within the parallel caps, so find,
		 * distance squared from point to line */

		/* distance squared to the cylinder axis */
		dsq = (pdx[0] * pdx[0] + pdx[1] * pdx[1] + pdx[2] * pdx[2]) - dot * dot / lengthsq;

		if (dsq > radius_sq)
		{
			return(-1.0f);
		}
		else
		{
			return(dsq);		// return distance squared to axis
		}
	}
}

static int gpencil_bone_looper(Object *ob, Bone *bone, void *data,
	int(*bone_func)(Object *, Bone *, void *))
{
	/* We want to apply the function bone_func to every bone
	 * in an armature -- feed bone_looper the first bone and
	 * a pointer to the bone_func and watch it go!. The int count
	 * can be useful for counting bones with a certain property
	 * (e.g. skinnable)
	 */
	int count = 0;

	if (bone) {
		/* only do bone_func if the bone is non null */
		count += bone_func(ob, bone, data);

		/* try to execute bone_func for the first child */
		count += gpencil_bone_looper(ob, bone->childbase.first, data, bone_func);

		/* try to execute bone_func for the next bone at this
		 * depth of the recursion.
		 */
		count += gpencil_bone_looper(ob, bone->next, data, bone_func);
	}

	return count;
}

static int bone_skinnable_cb(Object *UNUSED(ob), Bone *bone, void *datap)
{
	/* Bones that are deforming
	 * are regarded to be "skinnable" and are eligible for
	 * auto-skinning.
	 *
	 * This function performs 2 functions:
	 *
	 *   a) It returns 1 if the bone is skinnable.
	 *      If we loop over all bones with this
	 *      function, we can count the number of
	 *      skinnable bones.
	 *   b) If the pointer data is non null,
	 *      it is treated like a handle to a
	 *      bone pointer -- the bone pointer
	 *      is set to point at this bone, and
	 *      the pointer the handle points to
	 *      is incremented to point to the
	 *      next member of an array of pointers
	 *      to bones. This way we can loop using
	 *      this function to construct an array of
	 *      pointers to bones that point to all
	 *      skinnable bones.
	 */
	Bone ***hbone;
	int a, segments;
	struct { Object *armob; void *list; int heat;} *data = datap;

	if (!(bone->flag & BONE_HIDDEN_P)) {
		if (!(bone->flag & BONE_NO_DEFORM)) {
			if (data->heat && data->armob->pose && BKE_pose_channel_find_name(data->armob->pose, bone->name))
				segments = bone->segments;
			else
				segments = 1;

			if (data->list != NULL) {
				hbone = (Bone ***)&data->list;

				for (a = 0; a < segments; a++) {
					**hbone = bone;
					++*hbone;
				}
			}
			return segments;
		}
	}
	return 0;
}

static int vgroup_add_unique_bone_cb(Object *ob, Bone *bone, void *UNUSED(ptr))
{
	/* This group creates a vertex group to ob that has the
	 * same name as bone (provided the bone is skinnable).
	 * If such a vertex group already exist the routine exits.
	 */
	if (!(bone->flag & BONE_NO_DEFORM)) {
		if (!defgroup_find_name(ob, bone->name)) {
			BKE_object_defgroup_add_name(ob, bone->name);
			return 1;
		}
	}
	return 0;
}

static int dgroup_skinnable_cb(Object *ob, Bone *bone, void *datap)
{
	/* Bones that are deforming
	 * are regarded to be "skinnable" and are eligible for
	 * auto-skinning.
	 *
	 * This function performs 2 functions:
	 *
	 *   a) If the bone is skinnable, it creates
	 *      a vertex group for ob that has
	 *      the name of the skinnable bone
	 *      (if one doesn't exist already).
	 *   b) If the pointer data is non null,
	 *      it is treated like a handle to a
	 *      bDeformGroup pointer -- the
	 *      bDeformGroup pointer is set to point
	 *      to the deform group with the bone's
	 *      name, and the pointer the handle
	 *      points to is incremented to point to the
	 *      next member of an array of pointers
	 *      to bDeformGroups. This way we can loop using
	 *      this function to construct an array of
	 *      pointers to bDeformGroups, all with names
	 *      of skinnable bones.
	 */
	bDeformGroup ***hgroup, *defgroup = NULL;
	int a, segments;
	struct { Object *armob; void *list; int heat; } *data = datap;
	bArmature *arm = data->armob->data;

	if (!(bone->flag & BONE_HIDDEN_P)) {
		if (!(bone->flag & BONE_NO_DEFORM)) {
			if (data->heat && data->armob->pose && BKE_pose_channel_find_name(data->armob->pose, bone->name))
				segments = bone->segments;
			else
				segments = 1;

			//if (((arm->layer & bone->layer) && (bone->flag & BONE_SELECTED))) {
			if (arm->layer & bone->layer) {
				if (!(defgroup = defgroup_find_name(ob, bone->name))) {
					defgroup = BKE_object_defgroup_add_name(ob, bone->name);
				}
				else if (defgroup->flag & DG_LOCK_WEIGHT) {
					/* In case vgroup already exists and is locked, do not modify it here. See T43814. */
					defgroup = NULL;
				}
			}

			if (data->list != NULL) {
				hgroup = (bDeformGroup ***)&data->list;

				for (a = 0; a < segments; a++) {
					**hgroup = defgroup;
					++*hgroup;
				}
			}
			return segments;
		}
	}
	return 0;
}

/* This functions implements the automatic computation of vertex group weights */
static void gpencil_add_verts_to_dgroups(bContext *C,
	ReportList *reports, Depsgraph *depsgraph, Scene *scene, Object *ob, Object *ob_arm)
{
	bArmature *arm = ob_arm->data;
	Bone **bonelist, *bone;
	bDeformGroup **dgrouplist;
	bDeformGroup *dgroup;
	bPoseChannel *pchan;
	bGPdata *gpd = (bGPdata *)ob->data;
	bool is_multiedit = (bool)GPENCIL_MULTIEDIT_SESSIONS_ON(gpd);

	Mat4 bbone_array[MAX_BBONE_SUBDIV], *bbone = NULL;
	float(*root)[3], (*tip)[3], (*verts)[3];
	float *lensqr, *radsqr;
	int *selected;
	float weight;
	int numbones, i, j, segments = 0;
	struct { Object *armob; void *list; int heat; } looper_data;

	looper_data.armob = ob_arm;
	looper_data.heat = true;
	looper_data.list = NULL;

	/* count the number of skinnable bones */
	numbones = gpencil_bone_looper(ob, arm->bonebase.first, &looper_data, bone_skinnable_cb);

	if (numbones == 0)
		return;

	/* create an array of pointer to bones that are skinnable
	 * and fill it with all of the skinnable bones */
	bonelist = MEM_callocN(numbones * sizeof(Bone *), "bonelist");
	looper_data.list = bonelist;
	gpencil_bone_looper(ob, arm->bonebase.first, &looper_data, bone_skinnable_cb);

	/* create an array of pointers to the deform groups that
	 * correspond to the skinnable bones (creating them
	 * as necessary. */
	dgrouplist = MEM_callocN(numbones * sizeof(bDeformGroup *), "dgrouplist");

	looper_data.list = dgrouplist;
	gpencil_bone_looper(ob, arm->bonebase.first, &looper_data, dgroup_skinnable_cb);

	/* create an array of root and tip positions transformed into
	 * global coords */
	root = MEM_callocN(numbones * sizeof(float) * 3, "root");
	tip = MEM_callocN(numbones * sizeof(float) * 3, "tip");
	selected = MEM_callocN(numbones * sizeof(int), "selected");
	lensqr = MEM_callocN(numbones * sizeof(float), "lensqr");
	radsqr = MEM_callocN(numbones * sizeof(float), "radsqr");

	for (j = 0; j < numbones; j++) {
		bone = bonelist[j];
		dgroup = dgrouplist[j];

		/* handle bbone */
		if (segments == 0) {
			segments = 1;
			bbone = NULL;

			if ((ob_arm->pose) && (pchan = BKE_pose_channel_find_name(ob_arm->pose, bone->name))) {
				if (bone->segments > 1) {
					segments = bone->segments;
					b_bone_spline_setup(pchan, 1, bbone_array);
					bbone = bbone_array;
				}
			}
		}

		segments--;

		/* compute root and tip */
		if (bbone) {
			mul_v3_m4v3(root[j], bone->arm_mat, bbone[segments].mat[3]);
			if ((segments + 1) < bone->segments) {
				mul_v3_m4v3(tip[j], bone->arm_mat, bbone[segments + 1].mat[3]);
			}
			else {
				copy_v3_v3(tip[j], bone->arm_tail);
			}
		}
		else {
			copy_v3_v3(root[j], bone->arm_head);
			copy_v3_v3(tip[j], bone->arm_tail);
		}

		mul_m4_v3(ob_arm->obmat, root[j]);
		mul_m4_v3(ob_arm->obmat, tip[j]);

		selected[j] = 1;

		/* calculate len of bone squared */
		lensqr[j] = len_squared_v3v3(root[j], tip[j]);

		/* calculate radius squared */
		radsqr[j] = lensqr[j] / 6.0f;
	}

	/* loop all strokes */
	CTX_DATA_BEGIN(C, bGPDlayer *, gpl, editable_gpencil_layers)
	{
		bGPDframe *init_gpf = gpl->actframe;
		bGPDspoint *pt = NULL;

		if (is_multiedit) {
			init_gpf = gpl->frames.first;
		}

		for (bGPDframe *gpf = init_gpf; gpf; gpf = gpf->next) {
			if ((gpf == gpl->actframe) || ((gpf->flag & GP_FRAME_SELECT) && (is_multiedit))) {

				if (gpf == NULL)
					continue;

				for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
					/* skip strokes that are invalid for current view */
					if (ED_gpencil_stroke_can_use(C, gps) == false)
						continue;

					/* create verts array */
					verts = MEM_callocN(gps->totpoints * sizeof(*verts), __func__);

					/* transform stroke points to global space */
					for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
						copy_v3_v3(verts[i], &pt->x);
						mul_m4_v3(ob->obmat, verts[i]);
					}

					/* loop groups and assign weight */
					for (j = 0; j < numbones; j++) {
						int def_nr = BLI_findindex(&ob->defbase, dgrouplist[j]);
						if (def_nr < 0) {
							continue;
						}

						for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
							MDeformVert *dvert = &gps->dvert[i];
							float dist = test_point_in_cylynder(root[j], tip[j],
																lensqr[j], radsqr[j],
																pt);
							if (dist < 0) {
								/* if not in cylinder, check if inside sphere of extremes */
								weight = 0.0f;
								float rad = radsqr[j] * 0.75f;
								dist = len_squared_v3v3(root[j], &pt->x);
								if (dist < rad) {
									weight = interpf(0.0f, 0.9f, dist / rad);
								}
								else {
									dist = len_squared_v3v3(tip[j], &pt->x);
									if (dist < rad) {
										weight = interpf(0.0f, 0.9f, dist / rad);
									}
								}
							}
							else {
								/* inside bone cylinder */
								weight = 1.0f;
							}

							/* assign weight */
							BKE_gpencil_vgroup_add_point_weight(dvert, def_nr, weight);
						}
					}
					MEM_SAFE_FREE(verts);

				}
			}

			/* if not multiedit, exit loop*/
			if (!is_multiedit) {
				break;
			}
		}
	}
	CTX_DATA_END;

	/* free the memory allocated */
	MEM_SAFE_FREE(bonelist);
	MEM_SAFE_FREE(dgrouplist);
	MEM_SAFE_FREE(root);
	MEM_SAFE_FREE(tip);
	MEM_SAFE_FREE(lensqr);
	MEM_SAFE_FREE(radsqr);
	MEM_SAFE_FREE(selected);
}

static void gpencil_object_vgroup_calc_from_armature(bContext *C,
	ReportList *reports, Depsgraph *depsgraph, Scene *scene, Object *ob, Object *ob_arm,
	const int mode, const bool mirror)
{
	/* Lets try to create some vertex groups
	 * based on the bones of the parent armature.
	 */
	bArmature *arm = ob_arm->data;

	/* always create groups */
	const int defbase_tot = BLI_listbase_count(&ob->defbase);
	int defbase_add;
	/* Traverse the bone list, trying to create empty vertex
		* groups corresponding to the bone.
		*/
	defbase_add = gpencil_bone_looper(ob, arm->bonebase.first, NULL, vgroup_add_unique_bone_cb);

	if (defbase_add) {
		/* its possible there are DWeight's outside the range of the current
			* objects deform groups, in this case the new groups wont be empty */
		ED_vgroup_data_clamp_range(ob->data, defbase_tot);
	}
	
	if (mode == GP_ARMATURE_AUTO) {
		/* Traverse the bone list, trying to fill vertex groups
		 * with the corresponding vertice weights for which the
		 * bone is closest.
		 */
		gpencil_add_verts_to_dgroups(C, reports, depsgraph, scene, ob, ob_arm);
	}
}

/* ***************** Generate armature weights ************************** */
bool gpencil_generate_weights_poll(bContext *C)
{
	bGPdata *gpd = ED_gpencil_data_get_active(C);
	bGPDlayer *gpl = BKE_gpencil_layer_getactive(gpd);

	return (gpl != NULL);
}

static int gpencil_generate_weights_exec(bContext *C, wmOperator *op)
{
	Depsgraph *depsgraph = CTX_data_depsgraph(C);
	Scene *scene = CTX_data_scene(C);
	Object *ob = CTX_data_active_object(C);
	bGPdata *gpd = (bGPdata *)ob->data;

	int mode = RNA_enum_get(op->ptr, "mode");

	/* sanity checks */
	if (ELEM(NULL, ob, gpd))
		return OPERATOR_CANCELLED;

	/* get armature from modifier */
	GpencilModifierData *md = BKE_gpencil_modifiers_findByType(ob, eGpencilModifierType_Armature);
	if (md == NULL) {
		BKE_report(op->reports, RPT_ERROR,
				"The grease pencil object need an Armature modifier");
		return OPERATOR_CANCELLED;
	}

	ArmatureGpencilModifierData *mmd = (ArmatureGpencilModifierData *)md;
	if (mmd->object == NULL) {
		BKE_report(op->reports, RPT_ERROR,
			"Armature modifier is not valid or wrong defined");
		return OPERATOR_CANCELLED;
	}

	gpencil_object_vgroup_calc_from_armature(C, op->reports,depsgraph, scene,
											ob, mmd->object, mode, false);

	/* notifiers */
	DEG_id_tag_update(&gpd->id, OB_RECALC_OB | OB_RECALC_DATA);
	WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);

	return OPERATOR_FINISHED;
}

void GPENCIL_OT_generate_weights(wmOperatorType *ot)
{
	static const EnumPropertyItem mode_type[] = {
	{GP_ARMATURE_NAME, "NAME", 0, "With Empty Groups", ""},
	{GP_ARMATURE_AUTO, "AUTO", 0, "With Automatic Weights", ""},
	{0, NULL, 0, NULL, NULL}
	};

	/* identifiers */
	ot->name = "Generate Automatic Weights";
	ot->idname = "GPENCIL_OT_generate_weights";
	ot->description = "Generate automatic weights for armatures";

	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* callbacks */
	ot->exec = gpencil_generate_weights_exec;
	ot->poll = gpencil_generate_weights_poll;

	ot->prop = RNA_def_enum(ot->srna, "mode", mode_type, 0, "Mode", "");
}
