/*
 *  Digital Video Recorder
 *  Copyright (C) 2008 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdarg.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h> /* basename */

#include "htsstr.h"

#include "tvheadend.h"
#include "streaming.h"
#include "dvr.h"
#include "spawn.h"
#include "service.h"
#include "plumbing/tsfix.h"
#include "plumbing/globalheaders.h"
#include "htsp_server.h"
#include "atomic.h"
#include "intlconv.h"

#include "muxer.h"

/**
 *
 */
static void *dvr_thread(void *aux);
static void dvr_spawn_postproc(dvr_entry_t *de, const char *dvr_postproc);
static void dvr_thread_epilog(dvr_entry_t *de);


const static int prio2weight[6] = {
  [DVR_PRIO_IMPORTANT]   = 500,
  [DVR_PRIO_HIGH]        = 400,
  [DVR_PRIO_NORMAL]      = 300,
  [DVR_PRIO_LOW]         = 200,
  [DVR_PRIO_UNIMPORTANT] = 100,
  [DVR_PRIO_NOTSET]      = 0,
};

/**
 *
 */
void
dvr_rec_subscribe(dvr_entry_t *de)
{
  char buf[100];
  int weight;
  streaming_target_t *st;
  int flags;

  assert(de->de_s == NULL);

  if(de->de_pri < ARRAY_SIZE(prio2weight))
    weight = prio2weight[de->de_pri];
  else
    weight = 300;

  snprintf(buf, sizeof(buf), "DVR: %s", lang_str_get(de->de_title, NULL));

  if(dvr_entry_get_mc(de) == MC_PASS) {
    streaming_queue_init(&de->de_sq, SMT_PACKET);
    de->de_gh = NULL;
    de->de_tsfix = NULL;
    st = &de->de_sq.sq_st;
    flags = SUBSCRIPTION_RAW_MPEGTS;
  } else {
    streaming_queue_init(&de->de_sq, 0);
    de->de_gh = globalheaders_create(&de->de_sq.sq_st);
    st = de->de_tsfix = tsfix_create(de->de_gh);
    tsfix_set_start_time(de->de_tsfix, dvr_entry_get_start_time(de));
    flags = 0;
  }

  de->de_s = subscription_create_from_channel(de->de_channel, weight,
					      buf, st, flags,
					      NULL, NULL, NULL);

  tvhthread_create(&de->de_thread, NULL, dvr_thread, de);
}

/**
 *
 */
void
dvr_rec_unsubscribe(dvr_entry_t *de, int stopcode)
{
  assert(de->de_s != NULL);

  streaming_target_deliver(&de->de_sq.sq_st, streaming_msg_create(SMT_EXIT));
  
  pthread_join(de->de_thread, NULL);

  subscription_unsubscribe(de->de_s);
  de->de_s = NULL;

  if(de->de_tsfix)
    tsfix_destroy(de->de_tsfix);

  if(de->de_gh)
    globalheaders_destroy(de->de_gh);

  de->de_last_error = stopcode;
}


/**
 * Replace various chars with a dash
 */
static char *
cleanup_filename(char *s, dvr_config_t *cfg)
{
  int i, len = strlen(s);
  char *s1;

  s1 = intlconv_utf8safestr(cfg->dvr_charset_id, s, len * 2);
  if (s1 == NULL) {
    tvherror("dvr", "Unsupported charset %s using ASCII", cfg->dvr_charset);
    s1 = intlconv_utf8safestr(intlconv_charset_id("ASCII", 1, 1),
                             s, len * 2);
    if (s1 == NULL)
      return NULL;
  }
  s = s1;

  /* Do not create hidden files */
  if (s[0] == '.')
    s[0] = '_';

  for (i = 0, len = strlen(s); i < len; i++) {

    if(s[i] == '/')
      s[i] = '-';

    else if(cfg->dvr_whitespace_in_title &&
            (s[i] == ' ' || s[i] == '\t'))
      s[i] = '-';	

    else if(cfg->dvr_clean_title &&
            ((s[i] < 32) || (s[i] > 122) ||
             (strchr("/:\\<>|*?'\"", s[i]) != NULL)))
      s[i] = '_';
  }

  return s;
}

/**
 * Filename generator
 *
 * - convert from utf8
 * - avoid duplicate filenames
 *
 */
static int
pvr_generate_filename(dvr_entry_t *de, const streaming_start_t *ss)
{
  char fullname[PATH_MAX];
  char path[PATH_MAX];
  int tally = 0;
  struct stat st;
  char *filename, *s;
  struct tm tm;
  dvr_config_t *cfg;

  if (de == NULL)
    return -1;

  cfg = de->de_config;
  strncpy(path, cfg->dvr_storage, sizeof(path));
  path[sizeof(path)-1] = '\0';

  /* Remove trailing slash */
  if (path[strlen(path)-1] == '/')
    path[strlen(path)-1] = '\0';

  /* Append per-day directory */
  if (cfg->dvr_dir_per_day) {
    localtime_r(&de->de_start, &tm);
    strftime(fullname, sizeof(fullname), "%F", &tm);
    s = cleanup_filename(fullname, cfg);
    if (s == NULL)
      return -1;
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s", s);
    free(s);
  }

  /* Append per-channel directory */
  if (cfg->dvr_channel_dir) {
    char *chname = strdup(DVR_CH_NAME(de));
    s = cleanup_filename(chname, cfg);
    free(chname);
    if (s == NULL)
      return -1;
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s", s);
    free(s);
  }

  // TODO: per-brand, per-season

  /* Append per-title directory */
  if (cfg->dvr_title_dir) {
    char *title = strdup(lang_str_get(de->de_title, NULL));
    s = cleanup_filename(title, cfg);
    free(title);
    if (s == NULL)
      return -1;
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s", s);
    free(s);
  }

  if (makedirs(path, cfg->dvr_muxcnf.m_directory_permissions) != 0)
    return -1;
  
  /* Construct final name */
  dvr_make_title(fullname, sizeof(fullname), de);
  filename = cleanup_filename(fullname, cfg);
  if (filename == NULL)
    return -1;
  snprintf(fullname, sizeof(fullname), "%s/%s.%s",
	   path, filename, muxer_suffix(de->de_mux, ss));

  while(1) {
    if(stat(fullname, &st) == -1) {
      tvhlog(LOG_DEBUG, "dvr", "File \"%s\" -- %s -- Using for recording",
	     fullname, strerror(errno));
      break;
    }

    tvhlog(LOG_DEBUG, "dvr", "Overwrite protection, file \"%s\" exists", 
	   fullname);

    tally++;

    snprintf(fullname, sizeof(fullname), "%s/%s-%d.%s",
	     path, filename, tally, muxer_suffix(de->de_mux, ss));
  }
  free(filename);

  tvh_str_set(&de->de_filename, fullname);

  return 0;
}

/**
 *
 */
static void
dvr_rec_fatal_error(dvr_entry_t *de, const char *fmt, ...)
{
  char msgbuf[256];

  va_list ap;
  va_start(ap, fmt);

  vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
  va_end(ap);

  tvhlog(LOG_ERR, "dvr", 
	 "Recording error: \"%s\": %s",
	 de->de_filename ?: lang_str_get(de->de_title, NULL), msgbuf);
}


/**
 *
 */
static void
dvr_rec_set_state(dvr_entry_t *de, dvr_rs_state_t newstate, int error)
{
  int notify = 0;
  if(de->de_rec_state != newstate) {
    de->de_rec_state = newstate;
    notify = 1;
  }
  if(de->de_last_error != error) {
    de->de_last_error = error;
    notify = 1;
    if(error)
      de->de_errors++;
  }
  if (notify)
    idnode_notify_simple(&de->de_id);
}

/**
 *
 */
static int
dvr_rec_start(dvr_entry_t *de, const streaming_start_t *ss)
{
  const source_info_t *si = &ss->ss_si;
  const streaming_start_component_t *ssc;
  int i;
  dvr_config_t *cfg = de->de_config;
  muxer_container_type_t mc;

  if (!cfg) {
    dvr_rec_fatal_error(de, "Unable to determine config profile");
    return -1;
  }

  mc = dvr_entry_get_mc(de);

  de->de_mux = muxer_create(mc, &cfg->dvr_muxcnf);
  if(!de->de_mux) {
    dvr_rec_fatal_error(de, "Unable to create muxer");
    return -1;
  }

  if(pvr_generate_filename(de, ss) != 0) {
    dvr_rec_fatal_error(de, "Unable to create directories");
    return -1;
  }

  if(muxer_open_file(de->de_mux, de->de_filename)) {
    dvr_rec_fatal_error(de, "Unable to open file");
    return -1;
  }

  if(muxer_init(de->de_mux, ss, lang_str_get(de->de_title, NULL))) {
    dvr_rec_fatal_error(de, "Unable to init file");
    return -1;
  }

  if(cfg->dvr_tag_files && de->de_bcast) {
    if(muxer_write_meta(de->de_mux, de->de_bcast)) {
      dvr_rec_fatal_error(de, "Unable to write meta data");
      return -1;
    }
  }

  tvhlog(LOG_INFO, "dvr", "%s from "
	 "adapter: \"%s\", "
	 "network: \"%s\", mux: \"%s\", provider: \"%s\", "
	 "service: \"%s\"",
		
	 de->de_filename ?: lang_str_get(de->de_title, NULL),
	 si->si_adapter  ?: "<N/A>",
	 si->si_network  ?: "<N/A>",
	 si->si_mux      ?: "<N/A>",
	 si->si_provider ?: "<N/A>",
	 si->si_service  ?: "<N/A>");


  tvhlog(LOG_INFO, "dvr",
	 " #  %-16s  %-4s  %-10s  %-12s  %-11s  %-8s",
	 "type",
	 "lang",
	 "resolution",
	 "aspect ratio",
	 "sample rate",
	 "channels");

  for(i = 0; i < ss->ss_num_components; i++) {
    ssc = &ss->ss_components[i];

    char res[11];
    char asp[6];
    char sr[6];
    char ch[7];

    if(SCT_ISAUDIO(ssc->ssc_type)) {
      if(ssc->ssc_sri)
	snprintf(sr, sizeof(sr), "%d", sri_to_rate(ssc->ssc_sri));
      else
	strcpy(sr, "?");

      if(ssc->ssc_channels == 6)
	snprintf(ch, sizeof(ch), "5.1");
      else if(ssc->ssc_channels == 0)
	strcpy(ch, "?");
      else
	snprintf(ch, sizeof(ch), "%d", ssc->ssc_channels);
    } else {
      sr[0] = 0;
      ch[0] = 0;
    }

    if(SCT_ISVIDEO(ssc->ssc_type)) {
      if(ssc->ssc_width && ssc->ssc_height)
	snprintf(res, sizeof(res), "%dx%d",
		 ssc->ssc_width, ssc->ssc_height);
      else
	strcpy(res, "?");
    } else {
      res[0] = 0;
    }

    if(SCT_ISVIDEO(ssc->ssc_type)) {
      if(ssc->ssc_aspect_num &&  ssc->ssc_aspect_den)
	snprintf(asp, sizeof(asp), "%d:%d",
		 ssc->ssc_aspect_num, ssc->ssc_aspect_den);
      else
	strcpy(asp, "?");
    } else {
      asp[0] = 0;
    }

    tvhlog(LOG_INFO, "dvr",
	   "%2d  %-16s  %-4s  %-10s  %-12s  %-11s  %-8s  %s",
	   ssc->ssc_index,
	   streaming_component_type2txt(ssc->ssc_type),
	   ssc->ssc_lang,
	   res,
	   asp,
	   sr,
	   ch,
	   ssc->ssc_disabled ? "<disabled, no valid input>" : "");
  }

  return 0;
}


/**
 *
 */
static void *
dvr_thread(void *aux)
{
  dvr_entry_t *de = aux;
  dvr_config_t *cfg = de->de_config;
  streaming_queue_t *sq = &de->de_sq;
  streaming_message_t *sm;
  th_pkt_t *pkt;
  int run = 1;
  int started = 0;
  int comm_skip = cfg->dvr_skip_commercials;
  int commercial = COMMERCIAL_UNKNOWN;

  pthread_mutex_lock(&sq->sq_mutex);

  while(run) {
    sm = TAILQ_FIRST(&sq->sq_queue);
    if(sm == NULL) {
      pthread_cond_wait(&sq->sq_cond, &sq->sq_mutex);
      continue;
    }

    if (de->de_s && started) {
      pktbuf_t *pb = NULL;
      if (sm->sm_type == SMT_PACKET)
        pb = ((th_pkt_t*)sm->sm_data)->pkt_payload;
      else if (sm->sm_type == SMT_MPEGTS)
        pb = sm->sm_data;
      if (pb)
        atomic_add(&de->de_s->ths_bytes_out, pktbuf_len(pb));
    }

    TAILQ_REMOVE(&sq->sq_queue, sm, sm_link);

    pthread_mutex_unlock(&sq->sq_mutex);

    switch(sm->sm_type) {

    case SMT_PACKET:
      pkt = sm->sm_data;
      if(pkt->pkt_commercial == COMMERCIAL_YES)
	dvr_rec_set_state(de, DVR_RS_COMMERCIAL, 0);
      else
	dvr_rec_set_state(de, DVR_RS_RUNNING, 0);

      if(pkt->pkt_commercial == COMMERCIAL_YES && comm_skip)
	break;

      if(commercial != pkt->pkt_commercial)
	muxer_add_marker(de->de_mux);

      commercial = pkt->pkt_commercial;

      if(started) {
	muxer_write_pkt(de->de_mux, sm->sm_type, sm->sm_data);
	sm->sm_data = NULL;
      }
      break;

    case SMT_MPEGTS:
      if(started) {
	dvr_rec_set_state(de, DVR_RS_RUNNING, 0);
	muxer_write_pkt(de->de_mux, sm->sm_type, sm->sm_data);
	sm->sm_data = NULL;
      }
      break;

    case SMT_START:
      if(started &&
	 muxer_reconfigure(de->de_mux, sm->sm_data) < 0) {
	tvhlog(LOG_WARNING,
	       "dvr", "Unable to reconfigure \"%s\"",
	       de->de_filename ?: lang_str_get(de->de_title, NULL));

	// Try to restart the recording if the muxer doesn't
	// support reconfiguration of the streams.
	dvr_thread_epilog(de);
	started = 0;
      }

      if(!started) {
        pthread_mutex_lock(&global_lock);
        dvr_rec_set_state(de, DVR_RS_WAIT_PROGRAM_START, 0);
        if(dvr_rec_start(de, sm->sm_data) == 0) {
          started = 1;
          idnode_changed(&de->de_id);
          htsp_dvr_entry_update(de);
        }
        pthread_mutex_unlock(&global_lock);
      } 
      break;

    case SMT_STOP:
       if(sm->sm_code == SM_CODE_SOURCE_RECONFIGURED) {
	 // Subscription is restarting, wait for SMT_START

       } else if(sm->sm_code == 0) {
	 // Recording is completed

	de->de_last_error = 0;
	tvhlog(LOG_INFO, 
	       "dvr", "Recording completed: \"%s\"",
	       de->de_filename ?: lang_str_get(de->de_title, NULL));

	dvr_thread_epilog(de);
	started = 0;

      }else if(de->de_last_error != sm->sm_code) {
	 // Error during recording

	 dvr_rec_set_state(de, DVR_RS_ERROR, sm->sm_code);
	 tvhlog(LOG_ERR,
		"dvr", "Recording stopped: \"%s\": %s",
		de->de_filename ?: lang_str_get(de->de_title, NULL),
		streaming_code2txt(sm->sm_code));

	 dvr_thread_epilog(de);
	 started = 0;
      }
      break;

    case SMT_SERVICE_STATUS:
      if(sm->sm_code & TSS_PACKETS) {
	
      } else if(sm->sm_code & (TSS_GRACEPERIOD | TSS_ERRORS)) {

	int code = SM_CODE_UNDEFINED_ERROR;


	if(sm->sm_code & TSS_NO_DESCRAMBLER)
	  code = SM_CODE_NO_DESCRAMBLER;

	if(sm->sm_code & TSS_NO_ACCESS)
	  code = SM_CODE_NO_ACCESS;

	if(de->de_last_error != code) {
	  dvr_rec_set_state(de, DVR_RS_ERROR, code);
	  tvhlog(LOG_ERR,
		 "dvr", "Streaming error: \"%s\": %s",
		 de->de_filename ?: lang_str_get(de->de_title, NULL),
		 streaming_code2txt(code));
	}
      }
      break;

    case SMT_NOSTART:

      if(de->de_last_error != sm->sm_code) {
	dvr_rec_set_state(de, DVR_RS_PENDING, sm->sm_code);

	tvhlog(LOG_ERR,
	       "dvr", "Recording unable to start: \"%s\": %s",
	       de->de_filename ?: lang_str_get(de->de_title, NULL),
	       streaming_code2txt(sm->sm_code));
      }
      break;

    case SMT_GRACE:
    case SMT_SPEED:
    case SMT_SKIP:
    case SMT_SIGNAL_STATUS:
    case SMT_TIMESHIFT_STATUS:
      break;

    case SMT_EXIT:
      run = 0;
      break;
    }

    streaming_msg_free(sm);
    pthread_mutex_lock(&sq->sq_mutex);
  }
  pthread_mutex_unlock(&sq->sq_mutex);

  if(de->de_mux)
    dvr_thread_epilog(de);

  return NULL;
}


/**
 *
 */
static void
dvr_spawn_postproc(dvr_entry_t *de, const char *dvr_postproc)
{
  const char *fmap[256];
  char **args;
  char start[16];
  char stop[16];
  char *fbasename; /* filename dup for basename */
  int i;

  args = htsstr_argsplit(dvr_postproc);
  /* no arguments at all */
  if(!args[0]) {
    htsstr_argsplit_free(args);
    return;
  }

  fbasename = tvh_strdupa(de->de_filename);
  snprintf(start, sizeof(start), "%"PRItime_t, (time_t)dvr_entry_get_start_time(de));
  snprintf(stop, sizeof(stop),   "%"PRItime_t, (time_t)dvr_entry_get_stop_time(de));

  memset(fmap, 0, sizeof(fmap));
  fmap['f'] = de->de_filename; /* full path to recoding */
  fmap['b'] = basename(fbasename); /* basename of recoding */
  fmap['c'] = DVR_CH_NAME(de); /* channel name */
  fmap['C'] = de->de_creator; /* user who created this recording */
  fmap['t'] = lang_str_get(de->de_title, NULL); /* program title */
  fmap['d'] = lang_str_get(de->de_desc, NULL); /* program description */
  /* error message, empty if no error (FIXME:?) */
  fmap['e'] = tvh_strdupa(streaming_code2txt(de->de_last_error));
  fmap['S'] = start; /* start time, unix epoch */
  fmap['E'] = stop; /* stop time, unix epoch */
  // TODO: brand, season

  /* format arguments */
  for(i = 0; args[i]; i++) {
    char *s;

    s = htsstr_format(args[i], fmap);
    free(args[i]);
    args[i] = s;
  }
  
  spawnv(args[0], (void *)args);
    
  htsstr_argsplit_free(args);
}

/**
 *
 */
static void
dvr_thread_epilog(dvr_entry_t *de)
{
  muxer_close(de->de_mux);
  muxer_destroy(de->de_mux);
  de->de_mux = NULL;

  dvr_config_t *cfg = de->de_config;
  if(cfg && cfg->dvr_postproc && de->de_filename)
    dvr_spawn_postproc(de,cfg->dvr_postproc);
}
