/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Copyright (c) Jean Le Feuvre 2000-2005 
 *					All rights reserved
 *
 *  This file is part of GPAC / Scene Management sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *   
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 */

#include <gpac/scene_manager.h>
#include <gpac/constants.h>
#include <gpac/media_tools.h>
#include <gpac/bifs.h>
#ifndef GPAC_DISABLE_SVG
#include <gpac/laser.h>
#endif
#include <gpac/internal/scenegraph_dev.h>


#ifndef GPAC_READ_ONLY

static GF_MuxInfo *gf_sm_get_mux_info(GF_ESD *src)
{
	u32 i;
	GF_MuxInfo *mux = NULL;
	for (i=0; i<gf_list_count(src->extensionDescriptors); i++) {
		mux = gf_list_get(src->extensionDescriptors, i);
		if (mux->tag == GF_ODF_MUXINFO_TAG) return mux;
	}
	return NULL;
}

static void gf_sm_remove_mux_info(GF_ESD *src)
{
	u32 i;
	GF_MuxInfo *mux = NULL;
	for (i=0; i<gf_list_count(src->extensionDescriptors); i++) {
		mux = gf_list_get(src->extensionDescriptors, i);
		if (mux->tag == GF_ODF_MUXINFO_TAG) {
			gf_odf_desc_del((GF_Descriptor *)mux);
			gf_list_rem(src->extensionDescriptors, i);
			return;
		}
	}
}

static void gf_sm_finalize_mux(GF_ISOFile *mp4, GF_ESD *src, u32 offset_ts)
{
	u32 track, mts, ts;
	GF_MuxInfo *mux = gf_sm_get_mux_info(src);
	if (!mux && !offset_ts) return;
	track = gf_isom_get_track_by_id(mp4, src->ESID);
	if (!track) return;

	mts = gf_isom_get_media_timescale(mp4, track);
	ts = gf_isom_get_timescale(mp4);
	/*set track time offset*/
	if (mux) offset_ts += mux->startTime * mts / 1000;
	if (offset_ts) {
		u32 off = offset_ts * ts  / mts;
		u32 dur = (u32) gf_isom_get_media_duration(mp4, track);
		dur = dur * ts / mts;
		gf_isom_set_edit_segment(mp4, track, 0, off, 0, GF_ISOM_EDIT_EMPTY);
		gf_isom_set_edit_segment(mp4, track, off, dur, 0, GF_ISOM_EDIT_NORMAL);
	}
	/*set track interleaving ID*/
	if (mux && mux->GroupID) gf_isom_set_track_group(mp4, track, mux->GroupID);
}

static GF_Err gf_sm_import_ui_stream(GF_ISOFile *mp4, GF_ESD *src)
{
	GF_UIConfig *cfg;
	u32 len, i;
	GF_Err e;
	if (!src->slConfig) src->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
	src->slConfig->predefined = 2;
	src->slConfig->timestampResolution = 1000;
	if (!src->decoderConfig || !src->decoderConfig->decoderSpecificInfo) return GF_ODF_INVALID_DESCRIPTOR;
	if (src->decoderConfig->decoderSpecificInfo->tag == GF_ODF_UI_CFG_TAG) {
		cfg = (GF_UIConfig *) src->decoderConfig->decoderSpecificInfo;
		e = gf_odf_encode_ui_config(cfg, &src->decoderConfig->decoderSpecificInfo);
		gf_odf_desc_del((GF_Descriptor *) cfg);
		if (e) return e;
	} else if (src->decoderConfig->decoderSpecificInfo->tag != GF_ODF_DSI_TAG) {
		return GF_ODF_INVALID_DESCRIPTOR;
	}
	/*what's the media type for input sensor ??*/
	len = gf_isom_new_track(mp4, src->ESID, GF_ISOM_MEDIA_SCENE, 1000);
	if (!len) return gf_isom_last_error(mp4);
	gf_isom_set_track_enabled(mp4, len, 1);
	if (!src->ESID) src->ESID = gf_isom_get_track_id(mp4, len);
	return gf_isom_new_mpeg4_description(mp4, len, src, NULL, NULL, &i);
}

static GF_Err gf_sm_import_stream(GF_SceneManager *ctx, GF_ISOFile *mp4, GF_ESD *src, char *mediaSource)
{
	u32 track, di;
	GF_Err e;
	Bool isAudio, isVideo;
	char szName[1024];
	char *ext;
	GF_MediaImporter import;
	GF_MuxInfo *mux = NULL;

	/*no import if URL string*/
	if (src->URLString) {
		u32 mtype, track;
		if (!src->slConfig) src->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
		if (!src->decoderConfig) {
			fprintf(stdout, "ESD with URL string needs a decoder config with remote stream type to be encoded\n");
			return GF_BAD_PARAM;
		}
		/*however we still need a track to store the ESD ...*/
		switch (src->decoderConfig->streamType) {
		case GF_STREAM_VISUAL:
			mtype = GF_ISOM_MEDIA_VISUAL;
			break;
		case GF_STREAM_AUDIO:
			mtype = GF_ISOM_MEDIA_AUDIO;
			break;
		case GF_STREAM_MPEG7:
			mtype = GF_ISOM_MEDIA_MPEG7;
			break;
		case GF_STREAM_IPMP:
			mtype = GF_ISOM_MEDIA_IPMP;
			break;
		case GF_STREAM_OCI:
			mtype = GF_ISOM_MEDIA_OCI;
			break;
		case GF_STREAM_MPEGJ:
			mtype = GF_ISOM_MEDIA_MPEGJ;
			break;
		case GF_STREAM_INTERACT:
		case GF_STREAM_SCENE:
			mtype = GF_ISOM_MEDIA_SCENE;
			break;
		case GF_STREAM_TEXT:
			mtype = GF_ISOM_MEDIA_TEXT;
			break;
		default:
			fprintf(stdout, "Unsupported media type %d for ESD with URL string\n", src->decoderConfig->streamType);
			return GF_BAD_PARAM;
		}
		track = gf_isom_new_track(mp4, src->ESID, mtype, 1000);
		if (!src->ESID) src->ESID = gf_isom_get_track_id(mp4, track);
		return gf_isom_new_mpeg4_description(mp4, track, src, NULL, NULL, &di);
	}

	/*look for muxInfo*/
	mux = gf_sm_get_mux_info(src);

	/*special streams*/
	if (src->decoderConfig) {
		/*InputSensor*/
		if (src->decoderConfig->decoderSpecificInfo && (src->decoderConfig->decoderSpecificInfo->tag == GF_ODF_UI_CFG_TAG)) 
			src->decoderConfig->streamType = GF_STREAM_INTERACT;
		if (src->decoderConfig->streamType == GF_STREAM_INTERACT) return gf_sm_import_ui_stream(mp4, src);
	}


	/*OCR streams*/
	if (src->decoderConfig && src->decoderConfig->streamType == GF_STREAM_OCR) {
		track = gf_isom_new_track(mp4, src->ESID, GF_ISOM_MEDIA_OCR, 1000);
		if (!track) return gf_isom_last_error(mp4);
		gf_isom_set_track_enabled(mp4, track, 1);
		if (!src->ESID) src->ESID = gf_isom_get_track_id(mp4, track);
		if (!src->slConfig) src->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
		src->slConfig->predefined = 2;
		e = gf_isom_new_mpeg4_description(mp4, track, src, NULL, NULL, &di);
		if (e) return e;
		if (mux && mux->duration) 
			e = gf_isom_set_edit_segment(mp4, track, 0, mux->duration * gf_isom_get_timescale(mp4) / 1000, 0, GF_ISOM_EDIT_NORMAL);
		return e;
	}

	if (!mux) {
		/*if existing don't import (systems tracks)*/
		track = gf_isom_get_track_by_id(mp4, src->ESID);
		if (track) return GF_OK;
		if (mediaSource) {
			memset(&import, 0, sizeof(GF_MediaImporter));
			import.dest = mp4;
			import.trackID = src->ESID;
			import.orig = gf_isom_open(mediaSource, GF_ISOM_OPEN_READ, NULL);
			if (import.orig) {
				e = gf_media_import(&import);
				gf_isom_delete(import.orig);
				return e;
			}
		}
		return GF_OK;
	}

	if (!mux->file_name) return GF_OK;

	memset(&import, 0, sizeof(GF_MediaImporter));
	strcpy(szName, mux->file_name);
	ext = strrchr(szName, '.');	

	/*get track types for AVI*/
	if (ext && !strnicmp(ext, ".avi", 4)) {
		isAudio = isVideo = 0;
		if (ext && !stricmp(ext, ".avi#video")) isVideo = 1;
		else if (ext && !stricmp(ext, ".avi#audio")) isAudio = 1;
		else if (src->decoderConfig) {
			if (src->decoderConfig->streamType == GF_STREAM_VISUAL) isVideo = 1;
			else if (src->decoderConfig->streamType == GF_STREAM_AUDIO) isAudio = 1;
		}
		if (!isAudio && !isVideo) {
			fprintf(stdout, "Please specify video or audio for AVI import (file#audio, file#video)\n");
			return GF_NOT_SUPPORTED;
		}
		if (isVideo) import.trackID = 1;
		else import.trackID = 2;
		ext = strchr(ext, '#');
		if (ext) ext[0] = 0;
	}
	/*get track ID for MP4/others*/
	if (ext) {
		ext = strchr(ext, '#');
		if (ext) {
			import.trackID = atoi(ext+1);
			ext[0] = 0;
		}
	}

	import.streamFormat = mux->streamFormat;
	import.dest = mp4;
	import.esd = src;
	import.duration = mux->duration;
	import.flags = mux->import_flags;
	import.video_fps = mux->frame_rate;
	import.in_name = szName;
	e = gf_media_import(&import);
	if (e) return e;

	/*if desired delete input*/
	if (mux->delete_file) gf_delete_file(mux->file_name);
	return e;
}

static GF_Err gf_sm_import_stream_special(GF_SceneManager *ctx, GF_ESD *esd)
{
	GF_Err e;
	GF_MuxInfo *mux = gf_sm_get_mux_info(esd);
	if (!mux || !mux->file_name) return GF_OK;
	
	if (esd->decoderConfig && esd->decoderConfig->decoderSpecificInfo
		&& (esd->decoderConfig->decoderSpecificInfo->tag==GF_ODF_TEXT_CFG_TAG)) return GF_OK;

	e = GF_OK;
	/*SRT/SUB BIFS import if text node unspecified*/
	if (mux->textNode) {
		e = gf_sm_import_bifs_subtitle(ctx, esd, mux);
		gf_sm_remove_mux_info(esd);
	}
	return e;
}

static GF_Err gf_sm_import_specials(GF_SceneManager *ctx)
{
	GF_Err e;
	u32 i, j, n, m, k;
	GF_ESD *esd;
	GF_StreamContext *sc;

	for (i=0; i<gf_list_count(ctx->streams); i++) {
		sc = gf_list_get(ctx->streams, i);
		if (sc->streamType != GF_STREAM_OD) continue;
		esd = NULL;
		for (j=0; j<gf_list_count(sc->AUs); j++) {
			GF_AUContext *au = gf_list_get(sc->AUs, j);
			
			for (k=0; k<gf_list_count(au->commands); k++) {
				GF_ODCom *com = gf_list_get(au->commands, k);
				switch (com->tag) {
				case GF_ODF_OD_UPDATE_TAG:
				{
					GF_ODUpdate *odU = (GF_ODUpdate *)com;
					for (n=0; n<gf_list_count(odU->objectDescriptors); n++) {
						GF_ObjectDescriptor *od = gf_list_get(odU->objectDescriptors, n);
						for (m=0; m<gf_list_count(od->ESDescriptors); m++) {
							GF_ESD *imp_esd = gf_list_get(od->ESDescriptors, m);
							e = gf_sm_import_stream_special(ctx, imp_esd);
							if (e != GF_OK) return e;
						}
					}
				}
					break;
				case GF_ODF_ESD_UPDATE_TAG:
				{
					GF_ESDUpdate *esdU = (GF_ESDUpdate *)com;
					for (m=0; m<gf_list_count(esdU->ESDescriptors); m++) {
						GF_ESD *imp_esd = gf_list_get(esdU->ESDescriptors, m);
						e = gf_sm_import_stream_special(ctx, imp_esd);
						if (e != GF_OK) return e;
					}
				}
					break;
				}
			}
		}
	}
	return GF_OK;
}

/*locate stream in all OD updates/ESD updates (needed for systems tracks)*/
static GF_ESD *gf_sm_locate_esd(GF_SceneManager *ctx, u16 ES_ID)
{
	u32 i, j, n, m, k;
	GF_ESD *esd;
	GF_StreamContext *sc;
	if (!ES_ID) return NULL;

	for (i=0; i<gf_list_count(ctx->streams); i++) {
		sc = gf_list_get(ctx->streams, i);
		if (sc->streamType != GF_STREAM_OD) continue;
		esd = NULL;
		for (j=0; j<gf_list_count(sc->AUs); j++) {
			GF_AUContext *au = gf_list_get(sc->AUs, j);
			
			for (k=0; k<gf_list_count(au->commands); k++) {
				GF_ODCom *com = gf_list_get(au->commands, k);
				switch (com->tag) {
				case GF_ODF_OD_UPDATE_TAG:
				{
					GF_ODUpdate *odU = (GF_ODUpdate *)com;
					for (n=0; n<gf_list_count(odU->objectDescriptors); n++) {
						GF_ObjectDescriptor *od = gf_list_get(odU->objectDescriptors, n);
						for (m=0; m<gf_list_count(od->ESDescriptors); m++) {
							GF_ESD *imp_esd = gf_list_get(od->ESDescriptors, m);
							if (imp_esd->ESID == ES_ID) return imp_esd;
						}
					}
				}
					break;
				case GF_ODF_ESD_UPDATE_TAG:
				{
					GF_ESDUpdate *esdU = (GF_ESDUpdate *)com;
					for (m=0; m<gf_list_count(esdU->ESDescriptors); m++) {
						GF_ESD *imp_esd = gf_list_get(esdU->ESDescriptors, m);
						if (imp_esd->ESID == ES_ID) return imp_esd;
					}
				}
					break;
				}
			}
		}
	}
	return NULL;
}

static GF_Err gf_sm_encode_scene(GF_SceneManager *ctx, GF_ISOFile *mp4, char *logFile, u32 flags, u32 rap_freq, u32 scene_type)
{
	char *data;
	Bool is_in_iod, delete_desc, first_scene_id, rap_inband, rap_shadow;
	u32 i, j, di, dur, rate, time_slice, init_offset, data_len, count, track, last_rap, rap_delay;
	GF_Err e;
	FILE *logs;
	GF_InitialObjectDescriptor *iod;
	GF_AUContext *au;
	GF_ISOSample *samp;
	GF_StreamContext *sc;
	GF_ESD *esd;
	GF_BifsEncoder *bifs_enc;
	GF_LASeRCodec *lsr_enc;

	rap_inband = rap_shadow = 0;
	if (rap_freq) {
		if (flags & GF_SM_ENCODE_RAP_INBAND) {
			rap_inband = 1;
		} else {
			rap_shadow = 1;
		}
	}

	e = GF_OK;
	iod = (GF_InitialObjectDescriptor *) ctx->root_od;
	/*if no iod check we only have one bifs*/
	if (!iod) {
		count = 0;
		for (i=0; i<gf_list_count(ctx->streams); i++) {
			sc = gf_list_get(ctx->streams, i);
			if (sc->streamType == GF_STREAM_OD) count++;
		}
		if (!iod && count>1) return GF_NOT_SUPPORTED;
	}

	count = gf_list_count(ctx->streams);

	logs = NULL;
	if (logFile) logs = fopen(logFile, "wt");

	
	
	
	bifs_enc = NULL;
	if (!scene_type) {
		bifs_enc = gf_bifs_encoder_new(ctx->scene_graph);
		if (logs) gf_bifs_encoder_set_trace(bifs_enc, logs);
	}
	
	lsr_enc = NULL;
	if (scene_type==1) {
		lsr_enc = gf_laser_encoder_new(ctx->scene_graph);
		if (logs) gf_laser_set_trace(lsr_enc, logs);
	}

	delete_desc = 0;
	first_scene_id = 0;
	esd = NULL;

	/*configure streams*/
	for (i=0; i<count; i++) {
		GF_StreamContext *sc = gf_list_get(ctx->streams, i);
		esd = NULL;
		if (sc->streamType != GF_STREAM_SCENE) continue;
		/*NOT BIFS*/
		if (!scene_type && (sc->objectType > 2) ) continue;
		/*NOT LASeR*/
		if (scene_type && (sc->objectType != 0x09) ) continue;

		delete_desc = 0;
		esd = NULL;
		is_in_iod = 1;
		if (iod) {
			is_in_iod = 0;
			for (j=0; j<gf_list_count(iod->ESDescriptors); j++) {
				esd = gf_list_get(iod->ESDescriptors, j);
				if (esd->decoderConfig && esd->decoderConfig->streamType == GF_STREAM_SCENE) {
					if (!sc->ESID) sc->ESID = esd->ESID;
					if (sc->ESID == esd->ESID) {
						is_in_iod = 1;
						break;
					}
				}
				/*special BIFS direct import from NHNT*/
				else if (gf_list_count(iod->ESDescriptors)==1) {
					sc->ESID = esd->ESID;
					is_in_iod = 1;
					break;
				}
				esd = NULL;
			}
		}
		if (!esd && sc->ESID) esd = gf_sm_locate_esd(ctx, sc->ESID);

		if (!esd) {
			delete_desc = 1;
			esd = gf_odf_desc_esd_new(2);
			gf_odf_desc_del((GF_Descriptor *) esd->decoderConfig->decoderSpecificInfo);
			esd->decoderConfig->decoderSpecificInfo = NULL;
			esd->ESID = sc->ESID;
			esd->decoderConfig->streamType = GF_STREAM_SCENE;
		}

		/*special BIFS direct import from NHNT*/
		au = gf_list_get(sc->AUs, 0);
		if (gf_list_count(sc->AUs) == 1) {
			if (gf_list_count(au->commands) == 1) {
				GF_Command *com = gf_list_get(au->commands, 0);
				/*no root node, no protos (empty replace) - that's BIFS NHNT import*/
				if ((com->tag == GF_SG_SCENE_REPLACE) && !com->node && !gf_list_count(com->new_proto_list))
					au = NULL;
			}
		} 
		/*sanity check: remove first command if it is REPLACE SCENE BY NULL*/
		if (au && !au->timing && !au->timing_sec && (gf_list_count(au->commands) > 1)) {
			GF_Command *com = gf_list_get(au->commands, 0);
			if (com->tag==GF_SG_SCENE_REPLACE) {
				if (!com->node && !gf_list_count(com->new_proto_list) ) {
					gf_list_rem(au->commands, 0);
					gf_sg_command_del(com);
				}
			}
		}
		if (!au && !esd->URLString) {
			/*if not in IOD, the stream will be imported when encoding the OD stream*/
			if (!is_in_iod) continue;
			e = gf_sm_import_stream(ctx, mp4, esd, NULL);
			if (e) goto exit;
			gf_sm_finalize_mux(mp4, esd, 0);
			gf_isom_add_track_to_root_od(mp4, gf_isom_get_track_by_id(mp4, esd->ESID));
			continue;
		}

		if (!esd->slConfig) esd->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
		if (sc->timeScale) esd->slConfig->timestampResolution = sc->timeScale;
		if (!esd->slConfig->timestampResolution) esd->slConfig->timestampResolution = 1000;

		/*force scene dependencies (we cannot encode in 2 different scene contexts)*/
		if (!esd->dependsOnESID) {
			if (!first_scene_id) {
				esd->dependsOnESID = 0;
				first_scene_id = esd->ESID;
			} else {
				esd->dependsOnESID = first_scene_id;
			}
		}
		if (!esd->decoderConfig) esd->decoderConfig = (GF_DecoderConfig*)gf_odf_desc_new(GF_ODF_DCD_TAG);
		esd->decoderConfig->streamType = GF_STREAM_SCENE;

		/*create track*/
		track = gf_isom_new_track(mp4, sc->ESID, GF_ISOM_MEDIA_SCENE, esd->slConfig->timestampResolution);
		if (!track) {
			e = gf_isom_last_error(mp4);
			goto exit;
		}
		gf_isom_set_track_enabled(mp4, track, 1);
		if (!sc->ESID) sc->ESID = gf_isom_get_track_id(mp4, track);
		esd->ESID = sc->ESID;


		/*BIFS setup*/
		if (!scene_type) {
			GF_BIFSConfig *bcfg;
			Bool delete_bcfg = 0;

			if (!esd->decoderConfig->decoderSpecificInfo) {
				bcfg = (GF_BIFSConfig*)gf_odf_desc_new(GF_ODF_BIFS_CFG_TAG);
				delete_bcfg = 1;
			} else if (esd->decoderConfig->decoderSpecificInfo->tag == GF_ODF_BIFS_CFG_TAG) {
				bcfg = (GF_BIFSConfig *)esd->decoderConfig->decoderSpecificInfo;
			} else {
				bcfg = gf_odf_get_bifs_config(esd->decoderConfig->decoderSpecificInfo, esd->decoderConfig->objectTypeIndication);
				delete_bcfg = 1;
			}
			/*update NodeIDbits and co*/
			/*nodeID bits shall include NULL node*/
			if (!bcfg->nodeIDbits || (bcfg->nodeIDbits<gf_get_bit_size(ctx->max_node_id)) )
				bcfg->nodeIDbits = gf_get_bit_size(ctx->max_node_id);

			if (!bcfg->routeIDbits || (bcfg->routeIDbits != gf_get_bit_size(ctx->max_route_id)) )
				bcfg->routeIDbits = gf_get_bit_size(ctx->max_route_id);

			if (!bcfg->protoIDbits || (bcfg->protoIDbits != gf_get_bit_size(ctx->max_proto_id)) )
				bcfg->protoIDbits = gf_get_bit_size(ctx->max_proto_id);

			if (!bcfg->elementaryMasks) {
				bcfg->pixelMetrics = ctx->is_pixel_metrics;
				bcfg->pixelWidth = ctx->scene_width;
				bcfg->pixelHeight = ctx->scene_height;
			}

			/*this is for safety, otherwise some players may not understand NULL node*/
			if (!bcfg->nodeIDbits) bcfg->nodeIDbits = 1;
			gf_bifs_encoder_new_stream(bifs_enc, sc->ESID, bcfg, (flags & GF_SM_ENCODE_USE_NAMES) ? 1 : 0, 0);
			if (delete_bcfg) gf_odf_desc_del((GF_Descriptor *)bcfg);
			/*create final BIFS config*/
			if (esd->decoderConfig->decoderSpecificInfo) gf_odf_desc_del((GF_Descriptor *) esd->decoderConfig->decoderSpecificInfo);
			esd->decoderConfig->decoderSpecificInfo = (GF_DefaultDescriptor *) gf_odf_desc_new(GF_ODF_DSI_TAG);
			gf_bifs_encoder_get_config(bifs_enc, sc->ESID, &data, &data_len);
			esd->decoderConfig->decoderSpecificInfo->data = data;
			esd->decoderConfig->decoderSpecificInfo->dataLength = data_len;
			esd->decoderConfig->objectTypeIndication = gf_bifs_encoder_get_version(bifs_enc, sc->ESID);		
		} 
		/*LASeR setup*/
		if (scene_type==1) {
			GF_LASERConfig lsrcfg;

			if (!esd->decoderConfig->decoderSpecificInfo) {
				memset(&lsrcfg, 0, sizeof(GF_BIFSConfig));
				lsrcfg.tag = GF_ODF_LASER_CFG_TAG;
			} else if (esd->decoderConfig->decoderSpecificInfo->tag == GF_ODF_LASER_CFG_TAG) {
				memcpy(&lsrcfg, (GF_LASERConfig *)esd->decoderConfig->decoderSpecificInfo, sizeof(GF_LASERConfig));
			} else {
				gf_odf_get_laser_config(esd->decoderConfig->decoderSpecificInfo, &lsrcfg);
			}
			/*create final BIFS config*/
			if (esd->decoderConfig->decoderSpecificInfo) gf_odf_desc_del((GF_Descriptor *) esd->decoderConfig->decoderSpecificInfo);
			esd->decoderConfig->decoderSpecificInfo = (GF_DefaultDescriptor *) gf_odf_desc_new(GF_ODF_DSI_TAG);

			/*this is for safety, otherwise some players may not understand NULL node*/
			if (flags & GF_SM_ENCODE_USE_NAMES) lsrcfg.has_string_ids = 1;
			gf_laser_encoder_new_stream(lsr_enc, sc->ESID, &lsrcfg);
			/*get final config*/
			gf_laser_encoder_get_config(lsr_enc, sc->ESID, &data, &data_len);

			esd->decoderConfig->decoderSpecificInfo->data = data;
			esd->decoderConfig->decoderSpecificInfo->dataLength = data_len;
			esd->decoderConfig->objectTypeIndication = 0x09;
		}

		/*create stream description*/
		gf_isom_new_mpeg4_description(mp4, track, esd, NULL, NULL, &di);
		if (is_in_iod) {
			gf_isom_add_track_to_root_od(mp4, track);
			if (ctx->scene_width && ctx->scene_height)
				gf_isom_set_visual_info(mp4, track, di, ctx->scene_width, ctx->scene_height);
		}

		if (esd->URLString) continue;

		dur = esd->decoderConfig->avgBitrate = 0;
		esd->decoderConfig->bufferSizeDB = 0;
		esd->decoderConfig->maxBitrate = rate = time_slice = 0;

		last_rap = 0;
		rap_delay = rap_freq * esd->slConfig->timestampResolution / 1000;

		init_offset = 0;
		for (j=0; j<gf_list_count(sc->AUs); j++) {
			au = gf_list_get(sc->AUs, j);
			samp = gf_isom_sample_new();
			/*time in sec conversion*/
			if (au->timing_sec) au->timing = (u32) (au->timing_sec * esd->slConfig->timestampResolution);

			if (!j) init_offset = au->timing;

			samp->DTS = au->timing - init_offset;
			samp->IsRAP = au->is_rap;
			if (samp->IsRAP) last_rap = au->timing;

			/*inband RAP insertion*/
			if (rap_inband) {
				/*apply commands*/
				e = gf_sg_command_apply_list(ctx->scene_graph, au->commands, 0);
				if (samp->DTS - last_rap < rap_delay) {
					if (bifs_enc)
						e = gf_bifs_encode_au(bifs_enc, sc->ESID, au->commands, &samp->data, &samp->dataLength);
					else if (lsr_enc)
						e = gf_laser_encode_au(lsr_enc, sc->ESID, au->commands, 0, &samp->data, &samp->dataLength);
				} else {
					if (bifs_enc)
						e = gf_bifs_encoder_get_rap(bifs_enc, &samp->data, &samp->dataLength);
					else if (lsr_enc)
						e = gf_laser_encoder_get_rap(lsr_enc, &samp->data, &samp->dataLength);

					samp->IsRAP = 1;
					last_rap = samp->DTS;
				}
			} else {
				if (bifs_enc)
					e = gf_bifs_encode_au(bifs_enc, sc->ESID, au->commands, &samp->data, &samp->dataLength);
				else if (lsr_enc)
					e = gf_laser_encode_au(lsr_enc, sc->ESID, au->commands, 0, &samp->data, &samp->dataLength);
			}
			/*if no commands don't add the AU*/
			if (!e && samp->dataLength) e = gf_isom_add_sample(mp4, track, di, samp);

			dur = au->timing;
			esd->decoderConfig->avgBitrate += samp->dataLength;
			rate += samp->dataLength;
			if (esd->decoderConfig->bufferSizeDB<samp->dataLength) esd->decoderConfig->bufferSizeDB = samp->dataLength;
			if (samp->DTS - time_slice > esd->slConfig->timestampResolution) {
				if (esd->decoderConfig->maxBitrate < rate) esd->decoderConfig->maxBitrate = rate;
				rate = 0;
				time_slice = samp->DTS;
			}
			
			gf_isom_sample_del(&samp);
			if (e) goto exit;
		}

		if (dur) {
			esd->decoderConfig->avgBitrate *= esd->slConfig->timestampResolution * 8 / dur;
			esd->decoderConfig->maxBitrate *= 8;
		} else {
			esd->decoderConfig->avgBitrate = 0;
			esd->decoderConfig->maxBitrate = 0;
		}
		gf_isom_change_mpeg4_description(mp4, track, 1, esd);

		/*sync shadow generation*/
		if (rap_shadow) {
			last_rap = 0;
			for (j=0; j<gf_list_count(sc->AUs); j++) {
				GF_AUContext *au = gf_list_get(sc->AUs, j);
				e = gf_sg_command_apply_list(ctx->scene_graph, au->commands, 0);
				if (!j || (au->timing - last_rap < rap_delay) ) continue;

				samp = gf_isom_sample_new();
				samp->DTS = au->timing;
				samp->IsRAP = 1;
				last_rap = au->timing;
				/*RAP generation*/
				if (bifs_enc)
					e = gf_bifs_encoder_get_rap(bifs_enc, &samp->data, &samp->dataLength);

				if (!e) e = gf_isom_add_sample_shadow(mp4, track, samp);
				gf_isom_sample_del(&samp);
				if (e) goto exit;
			}
		}

		/*if offset add edit list*/
		gf_sm_finalize_mux(mp4, esd, init_offset);
		gf_isom_set_last_sample_duration(mp4, track, 0);

		if (delete_desc) {
			gf_odf_desc_del((GF_Descriptor *) esd);
			esd = NULL;
		}
	}

	/*to do - proper PL setup according to node used...*/
	gf_isom_set_pl_indication(mp4, GF_ISOM_PL_SCENE, 1);
	gf_isom_set_pl_indication(mp4, GF_ISOM_PL_GRAPHICS, 1);

exit:
	if (bifs_enc) gf_bifs_encoder_del(bifs_enc);
	else if (lsr_enc) gf_laser_encoder_del(lsr_enc);
	if (logFile) fclose(logs);
	if (esd && delete_desc) gf_odf_desc_del((GF_Descriptor *) esd);
	return e;
}


GF_Err gf_sm_encode_od(GF_SceneManager *ctx, GF_ISOFile *mp4, char *mediaSource)
{
	u32 i, j, n, m;
	GF_ESD *esd;
	GF_StreamContext *sc;
	u32 count, track, di, init_offset;
	u32 dur, rate, time_slice;
	Bool is_in_iod, delete_desc;

	GF_ISOSample *samp;
	GF_Err e;
	GF_ODCodec *codec;
	GF_InitialObjectDescriptor *iod;

	iod = (GF_InitialObjectDescriptor *) ctx->root_od;
	count = 0;
	for (i=0; i<gf_list_count(ctx->streams); i++) {
		sc = gf_list_get(ctx->streams, i);
		if (sc->streamType == GF_STREAM_OD) count++;
	}
	/*no OD stream, nothing to do*/
	if (!count) return GF_OK;
	if (!iod && count>1) return GF_NOT_SUPPORTED;

	esd = NULL;
	codec = NULL;
	delete_desc = 0;

	for (i=0; i<gf_list_count(ctx->streams); i++) {
		sc = gf_list_get(ctx->streams, i);
		if (sc->streamType != GF_STREAM_OD) continue;

		delete_desc = 0;
		esd = NULL;
		is_in_iod = 1;
		if (iod) {
			is_in_iod = 0;
			for (j=0; j<gf_list_count(iod->ESDescriptors); j++) {
				esd = gf_list_get(iod->ESDescriptors, j);
				if (esd->decoderConfig->streamType != GF_STREAM_OD){
					esd = NULL;
					continue;
				}
				if (!sc->ESID) sc->ESID = esd->ESID;
				if (sc->ESID == esd->ESID) {
					is_in_iod = 1;
					break;
				}
			}
		}
		if (!esd) esd = gf_sm_locate_esd(ctx, sc->ESID);
		if (!esd) {
			delete_desc = 1;
			esd = gf_odf_desc_esd_new(2);
			esd->ESID = sc->ESID;
			esd->decoderConfig->objectTypeIndication = 1;
			esd->decoderConfig->streamType = GF_STREAM_OD;
		}

		/*create OD track*/
		if (!esd->slConfig) esd->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
		if (sc->timeScale) esd->slConfig->timestampResolution = sc->timeScale;
		if (!esd->slConfig->timestampResolution) esd->slConfig->timestampResolution = 1000;
		track = gf_isom_new_track(mp4, sc->ESID, GF_ISOM_MEDIA_OD, esd->slConfig->timestampResolution);
		if (!sc->ESID) sc->ESID = gf_isom_get_track_id(mp4, track);
		gf_isom_set_track_enabled(mp4, track, 1);
		/*no DSI required*/
		/*create stream description*/
		gf_isom_new_mpeg4_description(mp4, track, esd, NULL, NULL, &di);
		/*add to root OD*/
		if (is_in_iod) gf_isom_add_track_to_root_od(mp4, track);

		codec = gf_odf_codec_new();

		dur = esd->decoderConfig->avgBitrate = 0;
		esd->decoderConfig->bufferSizeDB = 0;
		esd->decoderConfig->maxBitrate = rate = time_slice = 0;

		init_offset = 0;
		/*encode all samples and perform import - FIXME this is destructive...*/
		for (j=0; j<gf_list_count(sc->AUs); j++) {
			GF_AUContext *au = gf_list_get(sc->AUs, j);
			
			while (gf_list_count(au->commands) ) {
				GF_ODCom *com = gf_list_get(au->commands, 0);
				gf_list_rem(au->commands, 0);
				/*only updates commandes need to be parsed for import*/
				switch (com->tag) {
				case GF_ODF_OD_UPDATE_TAG:
				{
					GF_ODUpdate *odU = (GF_ODUpdate *)com;
					for (n=0; n<gf_list_count(odU->objectDescriptors); n++) {
						GF_ObjectDescriptor *od = gf_list_get(odU->objectDescriptors, n);
						for (m=0; m<gf_list_count(od->ESDescriptors); m++) {
							GF_ESD *imp_esd = gf_list_get(od->ESDescriptors, m);
							switch (imp_esd->tag) {
							case GF_ODF_ESD_TAG:
								e = gf_sm_import_stream(ctx, mp4, imp_esd, mediaSource);
								if (e) {
									fprintf(stdout, "Error importing stream %d\n", imp_esd->ESID);
									gf_odf_com_del(&com);
									goto err_exit;
								}
								gf_sm_finalize_mux(mp4, imp_esd, 0);
								break;
							case GF_ODF_ESD_REF_TAG:
							case GF_ODF_ESD_INC_TAG:
								break;
							default:
								fprintf(stdout, "Invalid descriptor in OD%d.ESDescr\n", od->objectDescriptorID);
								e = GF_BAD_PARAM;
								goto err_exit;
								break;
							}
						}
					}
				}
					break;
				case GF_ODF_ESD_UPDATE_TAG:
				{
					GF_ESDUpdate *esdU = (GF_ESDUpdate *)com;
					for (m=0; m<gf_list_count(esdU->ESDescriptors); m++) {
						GF_ESD *imp_esd = gf_list_get(esdU->ESDescriptors, m);
						switch (imp_esd->tag) {
						case GF_ODF_ESD_TAG:
							e = gf_sm_import_stream(ctx, mp4, imp_esd, mediaSource);
							if (e) {
								fprintf(stdout, "Error importing stream %d\n", imp_esd->ESID);
								gf_odf_com_del(&com);
								goto err_exit;
							}
							gf_sm_finalize_mux(mp4, imp_esd, 0);
							break;
						case GF_ODF_ESD_REF_TAG:
						case GF_ODF_ESD_INC_TAG:
							break;
						default:
							fprintf(stdout, "Invalid descriptor in ESDUpdate (OD %d)\n", esdU->ODID);
							e = GF_BAD_PARAM;
							goto err_exit;
							break;
						}
					}
				}
					break;
				}

				/*add to codec*/
				gf_odf_codec_add_com(codec, com);
			}
			e = gf_odf_codec_encode(codec);
			if (e) goto err_exit;

			/*time in sec conversion*/
			if (au->timing_sec) au->timing = (u32) (au->timing_sec * esd->slConfig->timestampResolution);

			if (!j) init_offset = au->timing;

			samp = gf_isom_sample_new();
			samp->DTS = au->timing - init_offset;
			samp->IsRAP = au->is_rap;
			e = gf_odf_codec_get_au(codec, &samp->data, &samp->dataLength);
			if (!e) e = gf_isom_add_sample(mp4, track, di, samp);

			dur = au->timing - init_offset;
			esd->decoderConfig->avgBitrate += samp->dataLength;
			rate += samp->dataLength;
			if (esd->decoderConfig->bufferSizeDB<samp->dataLength) esd->decoderConfig->bufferSizeDB = samp->dataLength;
			if (samp->DTS - time_slice > esd->slConfig->timestampResolution) {
				if (esd->decoderConfig->maxBitrate < rate) esd->decoderConfig->maxBitrate = rate;
				rate = 0;
				time_slice = samp->DTS;
			}

			gf_isom_sample_del(&samp);
			if (e) goto err_exit;
		}

		if (dur) {
			esd->decoderConfig->avgBitrate *= esd->slConfig->timestampResolution * 8 / dur;
			esd->decoderConfig->maxBitrate *= 8;
		} else {
			esd->decoderConfig->avgBitrate = 0;
			esd->decoderConfig->maxBitrate = 0;
		}
		gf_isom_change_mpeg4_description(mp4, track, 1, esd);

		gf_sm_finalize_mux(mp4, esd, init_offset);
		if (delete_desc) {
			gf_odf_desc_del((GF_Descriptor *) esd);
			esd = NULL;
		}
		esd = NULL;
		gf_isom_set_last_sample_duration(mp4, track, 0);
	}
	e = gf_isom_set_pl_indication(mp4, GF_ISOM_PL_OD, 1);
	
err_exit:
	if (codec) gf_odf_codec_del(codec);
	if (esd && delete_desc) gf_odf_desc_del((GF_Descriptor *) esd);
	return e;
}

GF_Err gf_sm_encode_to_file(GF_SceneManager *ctx, GF_ISOFile *mp4, char *logFile, char *mediaSource, u32 flags, u32 rap_freq)
{
	u32 i, count;
	GF_Descriptor *desc;
	GF_Err e;
	if (!ctx->scene_graph) return GF_BAD_PARAM;
	if (ctx->root_od && (ctx->root_od->tag != GF_ODF_IOD_TAG) && (ctx->root_od->tag != GF_ODF_OD_TAG)) return GF_BAD_PARAM;

	/*import specials, that is input remapping to BIFS*/
	e = gf_sm_import_specials(ctx);
	if (e) return e;


	/*encode BIFS*/
	e = gf_sm_encode_scene(ctx, mp4, logFile, flags, rap_freq, 0);
	if (e) return e;
	/*encode LASeR*/
	e = gf_sm_encode_scene(ctx, mp4, logFile, flags, rap_freq, 1);
	if (e) return e;
	/*then encode OD to setup all streams*/
	e = gf_sm_encode_od(ctx, mp4, mediaSource);
	if (e) return e;

	/*store iod*/
	if (ctx->root_od) {
		gf_isom_set_root_od_id(mp4, ctx->root_od->objectDescriptorID);
		if (ctx->root_od->URLString) gf_isom_set_root_od_url(mp4, ctx->root_od->URLString);
		count = gf_list_count(ctx->root_od->extensionDescriptors);
		for (i=0; i<count; i++) {
			desc = gf_list_get(ctx->root_od->extensionDescriptors, i);
			gf_isom_add_desc_to_root_od(mp4, desc);
		}
		count = gf_list_count(ctx->root_od->IPMP_Descriptors);
		for (i=0; i<count; i++) {
			desc = gf_list_get(ctx->root_od->IPMP_Descriptors, i);
			gf_isom_add_desc_to_root_od(mp4, desc);
		}
		count = gf_list_count(ctx->root_od->OCIDescriptors);
		for (i=0; i<count; i++) {
			desc = gf_list_get(ctx->root_od->OCIDescriptors, i);
			gf_isom_add_desc_to_root_od(mp4, desc);
		}
		if (ctx->root_od->tag==GF_ODF_IOD_TAG) {
			GF_InitialObjectDescriptor *iod = (GF_InitialObjectDescriptor*)ctx->root_od;
			if (iod->IPMPToolList) gf_isom_add_desc_to_root_od(mp4, (GF_Descriptor *) iod->IPMPToolList);
		}
		/*we assume all ESs described in bt/xmt input are used*/
	}

	/*set PLs*/
	if (ctx->root_od && ctx->root_od->tag==GF_ODF_IOD_TAG) {
		GF_InitialObjectDescriptor *iod =  (GF_InitialObjectDescriptor *)ctx->root_od;
		gf_isom_set_pl_indication(mp4, GF_ISOM_PL_OD, iod->OD_profileAndLevel);
		gf_isom_set_pl_indication(mp4, GF_ISOM_PL_SCENE, iod->scene_profileAndLevel);
		gf_isom_set_pl_indication(mp4, GF_ISOM_PL_GRAPHICS, iod->graphics_profileAndLevel);
		/*these are setup while importing*/
//		gf_isom_set_pl_indication(mp4, GF_ISOM_PL_VISUAL, iod->visual_profileAndLevel);
//		gf_isom_set_pl_indication(mp4, GF_ISOM_PL_AUDIO, iod->audio_profileAndLevel);
	}

	return GF_OK;
}

#endif
