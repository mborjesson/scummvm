/* ScummVM - Scumm Interpreter
 * Copyright (C) 2004 The ScummVM project
 *
 * The ReInherit Engine is (C)2000-2003 by Daniel Balsom.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * $Header$
 *
 */

// Scene management module
#include "saga.h"
#include "yslib.h"

#include "gfx.h"
#include "game_mod.h"
#include "animation.h"
#include "console_mod.h"
#include "cvar_mod.h"
#include "events_mod.h"
#include "actionmap.h"
#include "isomap_mod.h"
#include "script_mod.h"
#include "objectmap_mod.h"
#include "palanim_mod.h"
#include "render.h"
#include "rscfile_mod.h"
#include "script.h"
#include "text_mod.h"
#include "sound.h"
#include "music.h"

#include "scene_mod.h"
#include "scene.h"

namespace Saga {

static R_SCENE_MODULE SceneModule;

int SCENE_Register() {
	CVAR_Register_I(&SceneModule.scene_number, "scene", NULL, R_CVAR_READONLY, 0, 0);
	CVAR_RegisterFunc(CF_scenechange, "scene_change", "<Scene number>", R_CVAR_NONE, 1, 1, NULL);
	CVAR_RegisterFunc(CF_sceneinfo, "scene_info", NULL, R_CVAR_NONE, 0, 0, NULL);

	return R_SUCCESS;
}

int SCENE_Init() {
	R_GAME_SCENEDESC gs_desc;
	byte *scene_lut_p;
	size_t scene_lut_len;
	int result;
	int i;

	// Load game-specific scene data
	GAME_GetSceneInfo(&gs_desc);

	// Load scene module resource context
	result = GAME_GetFileContext(&SceneModule.scene_ctxt, R_GAME_RESOURCEFILE, 0);
	if (result != R_SUCCESS) {
		warning("Couldn't load scene resource context");
		return R_FAILURE;
	}

	// Initialize scene queue
	SceneModule.scene_queue = ys_dll_create();
	if (SceneModule.scene_queue == NULL) {
		return R_FAILURE;
	}

	// Load scene lookup table
	debug(0, "SCENE_Init(): Loading scene LUT from resource %u.", gs_desc.scene_lut_rn);
	result = RSC_LoadResource(SceneModule.scene_ctxt, gs_desc.scene_lut_rn, &scene_lut_p, &scene_lut_len);
	if (result != R_SUCCESS) {
		warning("Error: couldn't load scene LUT");
		return R_FAILURE;
	}

	SceneModule.scene_count = scene_lut_len / 2;
	SceneModule.scene_max = SceneModule.scene_count - 1;
	SceneModule.scene_lut = (int *)malloc(SceneModule.scene_max * sizeof *SceneModule.scene_lut);
	if (SceneModule.scene_lut == NULL) {
		warning("SCENE_Init(): Memory allocation failed");
		return R_MEM;
	}

	MemoryReadStream readS(scene_lut_p, scene_lut_len);

	for (i = 0; i < SceneModule.scene_max; i++) {
		SceneModule.scene_lut[i] = readS.readUint16LE();
	}

	free(scene_lut_p);

	if (gs_desc.first_scene != 0) {
		SceneModule.first_scene = gs_desc.first_scene;
	}

	debug(0, "SCENE_Init(): First scene set to %d.", SceneModule.first_scene);

	debug(0, "SCENE_Init(): LUT has %d entries.", SceneModule.scene_max);

	// Create scene module text list
	SceneModule.text_list = TEXT_CreateList();

	if (SceneModule.text_list == NULL) {
		warning("Error: Couldn't create scene text list");
		return R_FAILURE;
	}

	SceneModule.init = 1;

	return R_SUCCESS;
}

int SCENE_Shutdown() {
	if (SceneModule.init) {
		SCENE_End();
		free(SceneModule.scene_lut);
	}

	return R_SUCCESS;
}

int SCENE_Queue(R_SCENE_QUEUE *scene_queue) {
	assert(SceneModule.init);
	assert(scene_queue != NULL);

	ys_dll_add_tail(SceneModule.scene_queue, scene_queue, sizeof *scene_queue);

	return R_SUCCESS;
}

int SCENE_ClearQueue() {
	assert(SceneModule.init);

	ys_dll_delete_all(SceneModule.scene_queue);

	return R_SUCCESS;
}

int SCENE_Start() {
	YS_DL_NODE *node;
	R_SCENE_QUEUE *scene_qdat;

	assert(SceneModule.init);

	if (SceneModule.scene_loaded) {
		warning("Error: Can't start game...scene already loaded");
		return R_FAILURE;
	}

	if (SceneModule.in_game) {
		warning("Error: Can't start game...game already started");
		return R_FAILURE;
	}

	switch (GAME_GetGameType()) {
	case R_GAMETYPE_ITE:
		ITE_StartProc();
		break;
	case R_GAMETYPE_IHNM:
		IHNM_StartProc();
		break;
	default:
		warning("Error: Can't start game... gametype not supported");
		break;
	}

	// Load the head node in scene queue
	node = ys_dll_head(SceneModule.scene_queue);
	if (node == NULL) {
		return R_SUCCESS;
	}

	scene_qdat = (R_SCENE_QUEUE *)ys_dll_get_data(node);
	assert(scene_qdat != NULL);

	SCENE_Load(scene_qdat->scene_n, scene_qdat->load_flag, scene_qdat->scene_proc, scene_qdat->scene_desc);

	return R_SUCCESS;
}

int SCENE_Next() {
	YS_DL_NODE *node;
	R_SCENE_QUEUE *scene_qdat;

	assert(SceneModule.init);

	if (!SceneModule.scene_loaded) {
		warning("Error: Can't advance scene...no scene loaded");
		return R_FAILURE;
	}

	if (SceneModule.in_game) {
		warning("Error: Can't advance scene...game already started");
		return R_FAILURE;
	}

	SCENE_End();

	// Delete the current head node in scene queue
	node = ys_dll_head(SceneModule.scene_queue);
	if (node == NULL) {
		return R_SUCCESS;
	}

	ys_dll_delete(node);

	// Load the head node in scene queue
	node = ys_dll_head(SceneModule.scene_queue);
	if (node == NULL) {
		return R_SUCCESS;
	}

	scene_qdat = (R_SCENE_QUEUE *)ys_dll_get_data(node);
	assert(scene_qdat != NULL);

	SCENE_Load(scene_qdat->scene_n, scene_qdat->load_flag, scene_qdat->scene_proc, scene_qdat->scene_desc);

	return R_SUCCESS;
}

int SCENE_Skip() {
	YS_DL_NODE *node;
	YS_DL_NODE *prev_node;
	YS_DL_NODE *skip_node = NULL;

	R_SCENE_QUEUE *scene_qdat = NULL;
	R_SCENE_QUEUE *skip_qdat = NULL;

	assert(SceneModule.init);

	if (!SceneModule.scene_loaded) {
		warning("Error: Can't skip scene...no scene loaded");
		return R_FAILURE;
	}

	if (SceneModule.in_game) {
		warning("Error: Can't skip scene...game already started");
		return R_FAILURE;
	}

	// Walk down scene queue and try to find a skip target
	node = ys_dll_head(SceneModule.scene_queue);
	if (node == NULL) {
		warning("Error: Can't skip scene...no scenes in queue");
		return R_FAILURE;
	}

	for (node = ys_dll_next(node); node != NULL; node = ys_dll_next(node)) {
		scene_qdat = (R_SCENE_QUEUE *)ys_dll_get_data(node);
		assert(scene_qdat != NULL);

		if (scene_qdat->scene_skiptarget) {
			skip_node = node;
			skip_qdat = scene_qdat;
			break;
		}
	}

	// If skip target found, remove preceding scenes and load
	if (skip_node != NULL) {
		for (node = ys_dll_prev(skip_node); node != NULL; node = prev_node) {
			prev_node = ys_dll_prev(node);
			ys_dll_delete(node);
		}
		SCENE_End();
		SCENE_Load(skip_qdat->scene_n, skip_qdat->load_flag, skip_qdat->scene_proc, skip_qdat->scene_desc);
	}
	// Search for a scene to skip to

	return R_SUCCESS;
}

int SCENE_Change(int scene_num) {
	assert(SceneModule.init);

	if (!SceneModule.scene_loaded) {
		warning("Error: Can't change scene. No scene currently loaded. Game in invalid state");
		return R_FAILURE;
	}

	if ((scene_num < 0) || (scene_num > SceneModule.scene_max)) {
		warning("Error: Can't change scene. Invalid scene number");
		return R_FAILURE;
	}

	if (SceneModule.scene_lut[scene_num] == 0) {
		warning("Error: Can't change scene; invalid scene descriptor resource number (0)");
		return R_FAILURE;
	}

	SCENE_End();
	SCENE_Load(scene_num, BY_SCENE, defaultScene, NULL);

	return R_SUCCESS;
}

int SCENE_GetMode() {
	assert(SceneModule.init);

	return SceneModule.scene_mode;
}

int SCENE_GetZInfo(SCENE_ZINFO *zinfo) {
	assert(SceneModule.init);

	zinfo->begin_slope = SceneModule.desc.begin_slope;
	zinfo->end_slope = SceneModule.desc.end_slope;

	return R_SUCCESS;
}

int SCENE_GetBGInfo(SCENE_BGINFO *bginfo) {
	R_GAME_DISPLAYINFO di;
	int x, y;

	assert(SceneModule.init);

	bginfo->bg_buf = SceneModule.bg.buf;
	bginfo->bg_buflen = SceneModule.bg.buf_len;
	bginfo->bg_w = SceneModule.bg.w;
	bginfo->bg_h = SceneModule.bg.h;
	bginfo->bg_p = SceneModule.bg.p;

	GAME_GetDisplayInfo(&di);
	x = 0;
	y = 0;

	if (SceneModule.bg.w < di.logical_w) {
		x = (di.logical_w - SceneModule.bg.w) / 2;
	}

	if (SceneModule.bg.h < di.scene_h) {
		y = (di.scene_h - SceneModule.bg.h) / 2;
	}

	bginfo->bg_x = x;
	bginfo->bg_y = y;

	return R_SUCCESS;
}

int SCENE_GetBGPal(PALENTRY **pal) {
	assert(SceneModule.init);
	*pal = SceneModule.bg.pal;

	return R_SUCCESS;
}

int SCENE_GetBGMaskInfo(int *w, int *h, byte **buf, size_t *buf_len) {
	assert(SceneModule.init);

	if (!SceneModule.bg_mask.loaded) {
		return R_FAILURE;
	}

	*w = SceneModule.bg_mask.w;
	*h = SceneModule.bg_mask.h;
	*buf = SceneModule.bg_mask.buf;
	*buf_len = SceneModule.bg_mask.buf_len;

	return R_SUCCESS;
}

int SCENE_IsBGMaskPresent() {
	assert(SceneModule.init);

	return SceneModule.bg_mask.loaded;
}

int SCENE_GetInfo(R_SCENE_INFO *si) {
	assert(SceneModule.init);
	assert(si != NULL);

	si->text_list = SceneModule.text_list;

	return R_SUCCESS;
}

int SCENE_Load(int scene_num, int load_flag, R_SCENE_PROC scene_proc, R_SCENE_DESC *scene_desc_param) {
	R_SCENE_INFO scene_info;
	uint32 res_number = 0;
	int result;
	int i;

	assert(SceneModule.init);

	if (SceneModule.scene_loaded == 1) {
		warning("Error, a scene is already loaded");
		return R_FAILURE;
	}

	SceneModule.anim_list = ys_dll_create();
	SceneModule.scene_mode = 0;
	SceneModule.load_desc = 1;

	switch (load_flag) {
	case BY_RESOURCE:
		res_number = scene_num;
		break;
	case BY_SCENE:
		assert((scene_num > 0) && (scene_num < SceneModule.scene_max));
		res_number = SceneModule.scene_lut[scene_num];
		SceneModule.scene_number = scene_num;
		break;
	case BY_DESC:
		assert(scene_desc_param != NULL);
		assert(scene_desc_param->res_list != NULL);
		SceneModule.load_desc = 0;
		SceneModule.desc = *scene_desc_param;
		SceneModule.reslist = scene_desc_param->res_list;
		SceneModule.reslist_entries = scene_desc_param->res_list_ct;
		break;
	default:
		warning("Error: Invalid scene load flag");
		return R_FAILURE;
		break;
	}

	// Load scene descriptor and resource list resources
	if (SceneModule.load_desc) {

		SceneModule.scene_rn = res_number;
		assert(SceneModule.scene_rn != 0);
		debug(0, "Loading scene resource %u:", res_number);

		if (LoadSceneDescriptor(res_number) != R_SUCCESS) {
			warning("Error reading scene descriptor");
			return R_FAILURE;
		}

		if (LoadSceneResourceList(SceneModule.desc.res_list_rn) != R_SUCCESS) {
			warning("Error reading scene resource list");
			return R_FAILURE;
		}
	} else {
		debug(0, "Loading memory scene resource.");
	}

	// Load resources from scene resource list
	for (i = 0; i < SceneModule.reslist_entries; i++) {
		result = RSC_LoadResource(SceneModule.scene_ctxt, SceneModule.reslist[i].res_number,
								&SceneModule.reslist[i].res_data, &SceneModule.reslist[i].res_data_len);
		if (result != R_SUCCESS) {
			warning("Error: Allocation failure loading scene resource list");
			return R_FAILURE;
		}
	}

	// Process resources from scene resource list
	if (ProcessSceneResources() != R_SUCCESS) {
		warning("Error loading scene resources");
		return R_FAILURE;
	}

	// Load scene script data
	if (SceneModule.desc.script_num > 0) {
		if (_vm->_script->loadScript(SceneModule.desc.script_num) != R_SUCCESS) {
			warning("Error loading scene script");
			return R_FAILURE;
		}
	}

	SceneModule.scene_loaded = 1;

	if (scene_proc == NULL) {
		SceneModule.scene_proc = defaultScene;
	} else {
		SceneModule.scene_proc = scene_proc;
	}

	SCENE_GetInfo(&scene_info);

	SceneModule.scene_proc(SCENE_BEGIN, &scene_info);

	return R_SUCCESS;
}

int LoadSceneDescriptor(uint32 res_number) {
	byte *scene_desc_data;
	size_t scene_desc_len;
	int result;

	result = RSC_LoadResource(SceneModule.scene_ctxt, res_number, &scene_desc_data, &scene_desc_len);
	if (result != R_SUCCESS) {
		warning("Error: couldn't load scene descriptor");
		return R_FAILURE;
	}

	if (scene_desc_len != SAGA_SCENE_DESC_LEN) {
		warning("Error: scene descriptor length invalid");
		return R_FAILURE;
	}

	MemoryReadStream readS(scene_desc_data, scene_desc_len);

	SceneModule.desc.unknown0 = readS.readUint16LE();
	SceneModule.desc.res_list_rn = readS.readUint16LE();
	SceneModule.desc.end_slope = readS.readUint16LE();
	SceneModule.desc.begin_slope = readS.readUint16LE();
	SceneModule.desc.script_num = readS.readUint16LE();
	SceneModule.desc.scene_scriptnum = readS.readUint16LE();
	SceneModule.desc.start_scriptnum = readS.readUint16LE();
	SceneModule.desc.music_rn = readS.readSint16LE();

	RSC_FreeResource(scene_desc_data);

	return R_SUCCESS;
}

int LoadSceneResourceList(uint32 reslist_rn) {
	byte *resource_list;
	size_t resource_list_len;
	int result;
	int i;

	// Load the scene resource table
	result = RSC_LoadResource(SceneModule.scene_ctxt, reslist_rn, &resource_list, &resource_list_len);
	if (result != R_SUCCESS) {
		warning("Error: couldn't load scene resource list");
		return R_FAILURE;
	}

	MemoryReadStream readS(resource_list, resource_list_len);

	// Allocate memory for scene resource list 
	SceneModule.reslist_entries = resource_list_len / SAGA_RESLIST_ENTRY_LEN;
	debug(0, "Scene resource list contains %d entries.", SceneModule.reslist_entries);
	SceneModule.reslist = (R_SCENE_RESLIST *)calloc(SceneModule.reslist_entries, sizeof *SceneModule.reslist);

	if (SceneModule.reslist == NULL) {
		warning("Error: Memory allocation failed");
		return R_MEM;
	}

	// Load scene resource list from raw scene 
	// resource table
	debug(0, "Loading scene resource list...");

	for (i = 0; i < SceneModule.reslist_entries; i++) {
		SceneModule.reslist[i].res_number = readS.readUint16LE();
		SceneModule.reslist[i].res_type = readS.readUint16LE();
	}

	RSC_FreeResource(resource_list);

	return R_SUCCESS;
}

int ProcessSceneResources() {
	const byte *res_data;
	size_t res_data_len;
	const byte *pal_p;
	int i;

	// Process the scene resource list
	for (i = 0; i < SceneModule.reslist_entries; i++) {
		res_data = SceneModule.reslist[i].res_data;
		res_data_len = SceneModule.reslist[i].res_data_len;
		switch (SceneModule.reslist[i].res_type) {
		case SAGA_BG_IMAGE: // Scene background resource
			if (SceneModule.bg.loaded) {
				warning("ProcessSceneResources: Multiple background resources encountered");
				return R_FAILURE;
			}

			debug(0, "Loading background resource.");
			SceneModule.bg.res_buf = SceneModule.reslist[i].res_data;
			SceneModule.bg.res_len = SceneModule.reslist[i].res_data_len;
			SceneModule.bg.loaded = 1;

			if (_vm->decodeBGImage(SceneModule.bg.res_buf,
				SceneModule.bg.res_len,
				&SceneModule.bg.buf,
				&SceneModule.bg.buf_len,
				&SceneModule.bg.w,
				&SceneModule.bg.h) != R_SUCCESS) {
				warning("ProcessSceneResources: Error loading background resource: %u", SceneModule.reslist[i].res_number);
				return R_FAILURE;
			}

			pal_p = _vm->getImagePal(SceneModule.bg.res_buf, SceneModule.bg.res_len);
			memcpy(SceneModule.bg.pal, pal_p, sizeof SceneModule.bg.pal);
			SceneModule.scene_mode = R_SCENE_MODE_NORMAL;
			break;
		case SAGA_BG_MASK: // Scene background mask resource
			if (SceneModule.bg_mask.loaded) {
				warning("ProcessSceneResources: Duplicate background mask resource encountered");
			}
			debug(0, "Loading BACKGROUND MASK resource.");
			SceneModule.bg_mask.res_buf = SceneModule.reslist[i].res_data;
			SceneModule.bg_mask.res_len = SceneModule.reslist[i].res_data_len;
			SceneModule.bg_mask.loaded = 1;
			_vm->decodeBGImage(SceneModule.bg_mask.res_buf, SceneModule.bg_mask.res_len, &SceneModule.bg_mask.buf,
							&SceneModule.bg_mask.buf_len, &SceneModule.bg_mask.w, &SceneModule.bg_mask.h);
			break;
		case SAGA_OBJECT_NAME_LIST:
			debug(0, "Loading object name list resource...");
			OBJECTMAP_LoadNames(SceneModule.reslist[i].res_data, SceneModule.reslist[i].res_data_len);
			break;
		case SAGA_OBJECT_MAP:
			debug(0, "Loading object map resource...");
			if (OBJECTMAP_Load(res_data,
				res_data_len) != R_SUCCESS) {
				warning("Error loading object map resource");
				return R_FAILURE;
			}
			break;
		case SAGA_ACTION_MAP:
			debug(0, "Loading exit map resource...");
			if (_vm->_actionMap->loadMap(res_data, res_data_len) != R_SUCCESS) {
				warning("ProcessSceneResources: Error loading exit map resource");
				return R_FAILURE;
			}
			break;
		case SAGA_ISO_TILESET:
			if (SceneModule.scene_mode == R_SCENE_MODE_NORMAL) {
				warning("ProcessSceneResources: Isometric tileset incompatible with normal scene mode");
				return R_FAILURE;
			}

			debug(0, "Loading isometric tileset resource.");

			if (ISOMAP_LoadTileset(res_data, res_data_len) != R_SUCCESS) {
				warning("ProcessSceneResources: Error loading isometric tileset resource");
				return R_FAILURE;
			}

			SceneModule.scene_mode = R_SCENE_MODE_ISO;
			break;
		case SAGA_ISO_METAMAP:
			if (SceneModule.scene_mode == R_SCENE_MODE_NORMAL) {
				warning("ProcessSceneResources: Isometric metamap incompatible with normal scene mode");
				return R_FAILURE;
			}

			debug(0, "Loading isometric metamap resource.");

			if (ISOMAP_LoadMetamap(res_data, res_data_len) != R_SUCCESS) {
				warning("ProcessSceneResources: Error loading isometric metamap resource");
				return R_FAILURE;
			}

			SceneModule.scene_mode = R_SCENE_MODE_ISO;
			break;
		case SAGA_ISO_METATILESET:
			if (SceneModule.scene_mode == R_SCENE_MODE_NORMAL) {
				warning("ProcessSceneResources: Isometric metatileset incompatible with normal scene mode");
				return R_FAILURE;
			}

			debug(0, "Loading isometric metatileset resource.");

			if (ISOMAP_LoadMetaTileset(res_data, res_data_len) != R_SUCCESS) {
				warning("ProcessSceneResources: Error loading isometric tileset resource");
				return R_FAILURE;
			}

			SceneModule.scene_mode = R_SCENE_MODE_ISO;
			break;
		case SAGA_ANIM_1:
		case SAGA_ANIM_2:
		case SAGA_ANIM_3:
		case SAGA_ANIM_4:
		case SAGA_ANIM_5:
		case SAGA_ANIM_6:
		case SAGA_ANIM_7:
			{
				SCENE_ANIMINFO *new_animinfo;
				uint16 new_anim_id;

				debug(0, "Loading animation resource...");

				new_animinfo = (SCENE_ANIMINFO *)malloc(sizeof *new_animinfo);
				if (new_animinfo == NULL) {
					warning("ProcessSceneResources: Memory allocation error");
					return R_MEM;
				}

				if (_vm->_anim->load(SceneModule.reslist[i].res_data,
					SceneModule.reslist[i].res_data_len, &new_anim_id) != R_SUCCESS) {
					warning("ProcessSceneResources: Error loading animation resource");
					return R_FAILURE;
				}

				new_animinfo->anim_handle = new_anim_id;
				new_animinfo->anim_res_number =  SceneModule.reslist[i].res_number;
				ys_dll_add_tail(SceneModule.anim_list, new_animinfo, sizeof *new_animinfo);
				SceneModule.anim_entries++;
			}
			break;
		case SAGA_PAL_ANIM:
			debug(0, "Loading palette animation resource.");
			PALANIM_Load(SceneModule.reslist[i].res_data, SceneModule.reslist[i].res_data_len);
			break;
		default:
			warning("ProcessSceneResources: Encountered unknown resource type: %d", SceneModule.reslist[i].res_type);
			break;
		}
	}
	return R_SUCCESS;
}

int SCENE_Draw(R_SURFACE *dst_s) {
	R_GAME_DISPLAYINFO disp_info;
	R_BUFFER_INFO buf_info;
	R_POINT bg_pt;

	assert(SceneModule.init);

	_vm->_render->getBufferInfo(&buf_info);
	GAME_GetDisplayInfo(&disp_info);

	bg_pt.x = 0;
	bg_pt.y = 0;

	switch (SceneModule.scene_mode) {

	case R_SCENE_MODE_NORMAL:
		_vm->_gfx->bufToSurface(dst_s, buf_info.r_bg_buf, disp_info.logical_w,
						MAX(disp_info.scene_h, SceneModule.bg.h), NULL, &bg_pt);
		break;
	case R_SCENE_MODE_ISO:
		ISOMAP_Draw(dst_s);
		break;
	default:
		// Unknown scene mode
		return R_FAILURE;
		break;
	};

	return R_SUCCESS;
}

int SCENE_End() {
	R_SCENE_INFO scene_info;

	assert(SceneModule.init);

	if (SceneModule.scene_loaded != 1) {
		warning("SCENE_End(): No scene to end");
		return -1;
	}

	debug(0, "SCENE_End(): Ending scene...");

	SCENE_GetInfo(&scene_info);

	SceneModule.scene_proc(SCENE_END, &scene_info);

	if (SceneModule.desc.script_num > 0) {
		_vm->_script->freeScript();
	}

	// Free scene background
	if (SceneModule.bg.loaded) {
		free(SceneModule.bg.buf);
		SceneModule.bg.loaded = 0;
	}

	// Free scene background mask
	if (SceneModule.bg_mask.loaded) {
		free(SceneModule.bg_mask.buf);
		SceneModule.bg_mask.loaded = 0;
	}

	// Free scene resource list
	if (SceneModule.load_desc) {

		free(SceneModule.reslist);
	}

	// Free animation info list
	_vm->_anim->reset();

	PALANIM_Free();
	OBJECTMAP_Free();
	_vm->_actionMap->freeMap();

	ys_dll_destroy(SceneModule.anim_list);

	SceneModule.anim_entries = 0;

	EVENT_ClearList();
	TEXT_ClearList(SceneModule.text_list);

	SceneModule.scene_loaded = 0;

	return R_SUCCESS;
}

void CF_scenechange(int argc, char *argv[], void *refCon) {
	int scene_num = 0;

	if ((argc == 0) || (argc > 1)) {
		return;
	}

	scene_num = atoi(argv[0]);

	if ((scene_num < 1) || (scene_num > SceneModule.scene_max)) {
		CON_Print("Invalid scene number.");
		return;
	}

	SCENE_ClearQueue();

	if (SCENE_Change(scene_num) == R_SUCCESS) {
		CON_Print("Scene changed.");
	} else {
		CON_Print("Couldn't change scene!");
	}
}

void CF_sceneinfo(int argc, char *argv[], void *refCon) {
	const char *fmt = "%-20s %d";

	CON_Print(fmt, "Scene number:", SceneModule.scene_number);
	CON_Print(fmt, "Descriptor R#:", SceneModule.scene_rn);
	CON_Print("-------------------------");
	CON_Print(fmt, "Unknown:", SceneModule.desc.unknown0);
	CON_Print(fmt, "Resource list R#:", SceneModule.desc.res_list_rn);
	CON_Print(fmt, "End slope:", SceneModule.desc.end_slope);
	CON_Print(fmt, "Begin slope:", SceneModule.desc.begin_slope);
	CON_Print(fmt, "Script resource:", SceneModule.desc.script_num);
	CON_Print(fmt, "Scene script:", SceneModule.desc.scene_scriptnum);
	CON_Print(fmt, "Start script:", SceneModule.desc.start_scriptnum);
	CON_Print(fmt, "Music R#", SceneModule.desc.music_rn);
}

int initialScene(int param, R_SCENE_INFO *scene_info) {
	R_EVENT event;
	R_EVENT *q_event;
	int delay_time = 0;
	static PALENTRY current_pal[R_PAL_ENTRIES];
	PALENTRY *pal;

	switch (param) {
	case SCENE_BEGIN:
		_vm->_music->stop();
		_vm->_sound->stopVoice();

		// Fade palette to black from intro scene
		_vm->_gfx->getCurrentPal(current_pal);

		event.type = R_CONTINUOUS_EVENT;
		event.code = R_PAL_EVENT;
		event.op = EVENT_PALTOBLACK;
		event.time = 0;
		event.duration = PALETTE_FADE_DURATION;
		event.data = current_pal;

		delay_time += PALETTE_FADE_DURATION;

		q_event = EVENT_Queue(&event);

		// Activate user interface
		event.type = R_ONESHOT_EVENT;
		event.code = R_INTERFACE_EVENT;
		event.op = EVENT_ACTIVATE;
		event.time = 0;

		q_event = EVENT_Chain(q_event, &event);

		// Set first scene background w/o changing palette
		event.type = R_ONESHOT_EVENT;
		event.code = R_BG_EVENT;
		event.op = EVENT_DISPLAY;
		event.param = NO_SET_PALETTE;
		event.time = 0;

		q_event = EVENT_Chain(q_event, &event);

		// Fade in to first scene background palette
		SCENE_GetBGPal(&pal);

		event.type = R_CONTINUOUS_EVENT;
		event.code = R_PAL_EVENT;
		event.op = EVENT_BLACKTOPAL;
		event.time = delay_time;
		event.duration = PALETTE_FADE_DURATION;
		event.data = pal;

		q_event = EVENT_Chain(q_event, &event);

		event.code = R_PALANIM_EVENT;
		event.op = EVENT_CYCLESTART;
		event.time = 0;

		q_event = EVENT_Chain(q_event, &event);

		_vm->_anim->setFlag(0, ANIM_LOOP);
		_vm->_anim->play(0, delay_time);

		debug(0, "InitialSceneproc(): Scene started");
		break;
	case SCENE_END:
		break;
	default:
		warning("Illegal scene procedure parameter");
		break;
	}

	return 0;
}

int defaultScene(int param, R_SCENE_INFO *scene_info) {
	R_EVENT event;

	switch (param) {
	case SCENE_BEGIN:
		// Set scene background
		event.type = R_ONESHOT_EVENT;
		event.code = R_BG_EVENT;
		event.op = EVENT_DISPLAY;
		event.param = SET_PALETTE;
		event.time = 0;

		EVENT_Queue(&event);

		// Activate user interface
		event.type = R_ONESHOT_EVENT;
		event.code = R_INTERFACE_EVENT;
		event.op = EVENT_ACTIVATE;
		event.time = 0;

		EVENT_Queue(&event);

		// Begin palette cycle animation if present
		event.type = R_ONESHOT_EVENT;
		event.code = R_PALANIM_EVENT;
		event.op = EVENT_CYCLESTART;
		event.time = 0;

		EVENT_Queue(&event);
		break;
	case SCENE_END:
		break;
	default:
		warning("Illegal scene procedure parameter");
		break;
	}

	return 0;
}

} // End of namespace Saga
