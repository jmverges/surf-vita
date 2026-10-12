/* H.264 decode on the Vita hardware AVC decoder. See decoder.h for threading.
 *
 * Setup follows the documented SceAvcdec flow (init library, query decoder
 * memory, allocate it physically contiguous, create decoder) and outputs
 * RGBA8888 straight into vita2d textures (vita2d allocates texture memory in
 * CDRAM, which is physically contiguous and GPU mapped). */
#include "decoder.h"
#include "session.h"

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define NUM_TEXTURES   3
#define QUEUE_CAP      8
#define NUM_REF_FRAMES 4 /* as vita-moonlight; the decoder holds ~4 pictures for output, fewer corrupts them */
#define AU_PADDING     64
#define SEQ_FIFO       16

typedef struct {
  uint8_t *data;
  size_t len;
  int idr;
  uint16_t width, height;
  uint32_t sequence;
} au_item;

typedef struct {
  vita2d_texture *tex;
  int width, height;
  uint32_t sequence;
} out_slot;

static struct {
  int initialized;
  int lib_inited;
  int sysmodule_loaded;
  SceUID frame_block;
  SceAvcdecCtrl ctrl;
  int ctrl_created;

  pthread_t thread;
  int thread_started;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  int quit;

  /* input queue (lock) */
  au_item queue[QUEUE_CAP];
  int q_head, q_count;
  int waiting_idr;       /* producer side: drop until IDR */
  uint32_t generation;   /* bumped by dec_reset */

  /* output (lock) */
  out_slot slots[NUM_TEXTURES];
  int ready;             /* newest decoded, not yet taken by renderer */
  int shown;             /* taken by renderer */
  int writing;           /* being written by decoder */
  int last_returned;     /* to compute is_new */

  dec_stats stats;
} D;

static void free_item(au_item *it) {
  free(it->data);
  memset(it, 0, sizeof(*it));
}

static void queue_clear_locked(void) {
  while (D.q_count > 0) {
    free_item(&D.queue[D.q_head]);
    D.q_head = (D.q_head + 1) % QUEUE_CAP;
    D.q_count--;
    D.stats.dropped++;
  }
  D.q_head = 0;
}

void dec_submit(const uint8_t *au, size_t len, int idr, uint16_t width,
                uint16_t height, uint32_t sequence) {
  if (!D.initialized || !au || len == 0) return;

  pthread_mutex_lock(&D.lock);
  D.stats.submitted++;

  if (width > DEC_MAX_WIDTH || height > DEC_MAX_HEIGHT || width == 0 || height == 0) {
    D.stats.dropped++;
    pthread_mutex_unlock(&D.lock);
    return;
  }

  if (idr) {
    /* A newer IDR makes everything queued before it useless. */
    queue_clear_locked();
    D.waiting_idr = 0;
  } else if (D.waiting_idr) {
    D.stats.dropped++;
    pthread_mutex_unlock(&D.lock);
    return;
  } else if (D.q_count == QUEUE_CAP) {
    /* Decoder can't keep up. Non-IDR AUs can't be skipped individually
     * (references), so drop everything and resync on the next IDR. */
    queue_clear_locked();
    D.waiting_idr = 1;
    D.stats.dropped++;
    pthread_mutex_unlock(&D.lock);
    return;
  }
  pthread_mutex_unlock(&D.lock);

  /* Copy outside the lock. */
  uint8_t *copy = malloc(len + AU_PADDING);
  if (!copy) {
    pthread_mutex_lock(&D.lock);
    D.stats.dropped++;
    D.waiting_idr = 1;
    pthread_mutex_unlock(&D.lock);
    return;
  }
  memcpy(copy, au, len);
  memset(copy + len, 0, AU_PADDING);

  pthread_mutex_lock(&D.lock);
  if (idr) {
    queue_clear_locked(); /* anything that slipped in meanwhile */
    D.waiting_idr = 0;
  } else if (D.waiting_idr || D.q_count == QUEUE_CAP) {
    if (!D.waiting_idr) queue_clear_locked();
    D.waiting_idr = 1;
    D.stats.dropped++;
    pthread_mutex_unlock(&D.lock);
    free(copy);
    return;
  }
  au_item *it = &D.queue[(D.q_head + D.q_count) % QUEUE_CAP];
  it->data = copy;
  it->len = len;
  it->idr = idr;
  it->width = width;
  it->height = height;
  it->sequence = sequence;
  D.q_count++;
  pthread_cond_signal(&D.cond);
  pthread_mutex_unlock(&D.lock);
}

void dec_reset(void) {
  if (!D.initialized) return;
  pthread_mutex_lock(&D.lock);
  queue_clear_locked();
  D.waiting_idr = 1;
  D.generation++;
  pthread_mutex_unlock(&D.lock);
}

static int pick_free_slot_locked(void) {
  for (int i = 0; i < NUM_TEXTURES; i++)
    if (i != D.ready && i != D.shown) return i;
  return -1; /* cannot happen with 3 slots and one writer */
}

volatile int dec_in_decode;
volatile uint32_t dec_last_submitted_seq; /* newest AU given to the decoder */
volatile uint32_t dec_last_output_seq;    /* AU of the newest picture out */
volatile int dec_pending;                 /* AUs decoded with no picture out yet */
volatile unsigned long long dec_last_au_us; /* when the last AU reached the decoder */
volatile int dec_lowdelay_ret = 1; /* debug: result of the low-delay call */

/* Not in the VitaSDK headers (SonicMastr/libraries avcdec.h); exported by
 * SceVideodec. Mode 0 = lowest output delay. */
 /* debug: inside sceAvcdecDecode */

static void *decoder_thread(void *arg) {
  (void)arg;
  int need_idr = 1;            /* decoder side: skip until an IDR decodes */
  uint32_t seq_fifo[SEQ_FIFO]; /* input sequences awaiting output (reorder/delay) */
  int seq_n = 0;
  uint32_t seen_generation = 0;

  for (;;) {
    pthread_mutex_lock(&D.lock);
    while (!D.quit && D.q_count == 0) pthread_cond_wait(&D.cond, &D.lock);
    if (D.quit) {
      pthread_mutex_unlock(&D.lock);
      break;
    }
    au_item it = D.queue[D.q_head];
    memset(&D.queue[D.q_head], 0, sizeof(au_item));
    D.q_head = (D.q_head + 1) % QUEUE_CAP;
    D.q_count--;
    if (D.generation != seen_generation) {
      seen_generation = D.generation;
      need_idr = 1;
      seq_n = 0;
    }
    int slot = pick_free_slot_locked();
    D.writing = slot;
    pthread_mutex_unlock(&D.lock);

    if (need_idr && !it.idr) {
      pthread_mutex_lock(&D.lock);
      D.stats.dropped++;
      D.writing = -1;
      pthread_mutex_unlock(&D.lock);
      free_item(&it);
      continue;
    }
    if (it.idr) seq_n = 0;

    out_slot *out = &D.slots[slot];
    SceAvcdecPicture picture;
    SceAvcdecPicture *pictures[1] = {&picture};
    SceAvcdecArrayPicture array;
    SceAvcdecAu au;
    memset(&picture, 0, sizeof(picture));
    memset(&array, 0, sizeof(array));
    memset(&au, 0, sizeof(au));

    picture.size = sizeof(picture);
    picture.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
    picture.frame.framePitch = vita2d_texture_get_stride(out->tex) / 4; /* pixels */
    picture.frame.frameWidth = ALIGN_UP(it.width, 16);
    picture.frame.frameHeight = ALIGN_UP(it.height, 16);
    picture.frame.pPicture[0] = vita2d_texture_get_datap(out->tex);

    array.numOfElm = 1;
    array.pPicture = pictures;

    au.es.pBuf = it.data;
    au.es.size = it.len;
    /* The AU's sequence rides along as its timestamp, so each output picture
     * says which AU it is (the decoder holds several back). */
    au.pts.upper = au.dts.upper = 0;
    au.pts.lower = au.dts.lower = it.sequence;
    dec_last_submitted_seq = it.sequence;

    dec_last_au_us = sceKernelGetProcessTimeWide();
    dec_in_decode = 1;
    int ret = sceAvcdecDecode(&D.ctrl, &au, &array);
    dec_in_decode = 0;

    pthread_mutex_lock(&D.lock);
    D.writing = -1;
    if (ret < 0) {
      D.stats.errors++;
      D.stats.last_error = (uint32_t)ret;
      need_idr = 1;
      seq_n = 0;
    } else {
      need_idr = 0;
      if (seq_n < SEQ_FIFO) seq_fifo[seq_n++] = it.sequence;
      /* The backend paces frames on presentation reports. The hardware decoder
       * holds the first pictures back, so an unanswered frame would stall the
       * stream (2 frames, then nothing): report each accepted AU right away. */
      if (array.numOfOutput < 1) sv_presented(it.sequence);
      D.stats.no_output += array.numOfOutput < 1;
      if (array.numOfOutput >= 1 && seq_n > 0) {
        /* Output is in decode order for constrained baseline; if the decoder
         * delays output, the picture belongs to the oldest pending AU. */
        uint32_t out_seq = seq_fifo[0];
        memmove(seq_fifo, seq_fifo + 1, (size_t)(seq_n - 1) * sizeof(uint32_t));
        seq_n--;
        if (picture.info.pts.upper == 0) out_seq = picture.info.pts.lower; /* exact */
        dec_last_output_seq = out_seq;
        out->width = it.width;
        out->height = it.height;
        out->sequence = out_seq;
        if (D.ready >= 0) D.stats.dropped++; /* superseded before shown */
        D.ready = slot;
        D.stats.decoded++;
      }
    }
    dec_pending = seq_n;
    D.stats.waiting_idr = D.waiting_idr || need_idr;
    pthread_mutex_unlock(&D.lock);
    free_item(&it);
  }
  return NULL;
}

int dec_latest(vita2d_texture **tex, int *width, int *height,
               uint32_t *sequence, int *is_new) {
  if (!D.initialized) return 0;
  pthread_mutex_lock(&D.lock);
  if (D.ready >= 0) {
    /* The previously shown texture is free now: the renderer waited for the
     * GPU after drawing it (see decoder.h contract). */
    D.shown = D.ready;
    D.ready = -1;
  }
  int s = D.shown;
  int have = s >= 0;
  if (have) {
    *tex = D.slots[s].tex;
    *width = D.slots[s].width;
    *height = D.slots[s].height;
    *sequence = D.slots[s].sequence;
    /* A new frame always lands in a different slot than the one shown. */
    *is_new = (s != D.last_returned);
    D.last_returned = s;
  }
  pthread_mutex_unlock(&D.lock);
  return have;
}

void dec_get_stats(dec_stats *out) {
  pthread_mutex_lock(&D.lock);
  *out = D.stats;
  pthread_mutex_unlock(&D.lock);
}

int dec_init(void) {
  int ret;
  if (D.initialized) return 0;
  memset(&D, 0, sizeof(D));
  D.frame_block = -1;
  D.ready = D.shown = D.writing = D.last_returned = -1;
  D.waiting_idr = 1;

  /* The video decoder library is part of the system already (vita-moonlight
   * calls sceVideodecInitLibrary directly); on hardware loading the AVCDEC
   * sysmodule fails with SCE_SYSMODULE_ERROR_INVALID_VALUE, so it's optional. */
  ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
  if (ret < 0)
    printf("decoder: load AVCDEC module 0x%08x (ignored)\n", ret);
  else
    D.sysmodule_loaded = 1;

  /* Textures: RGBA8888 (ABGR in GXM terms), CDRAM (physically contiguous). */
  vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
  for (int i = 0; i < NUM_TEXTURES; i++) {
    D.slots[i].tex = vita2d_create_empty_texture_format(
        DEC_MAX_WIDTH, DEC_MAX_HEIGHT, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (!D.slots[i].tex) {
      printf("decoder: texture %d alloc failed\n", i);
      ret = SCE_AVCDEC_ERROR_OUT_OF_MEMORY;
      goto fail;
    }
    memset(vita2d_texture_get_datap(D.slots[i].tex), 0,
           vita2d_texture_get_stride(D.slots[i].tex) * DEC_MAX_HEIGHT);
  }

  SceVideodecQueryInitInfoHwAvcdec init;
  memset(&init, 0, sizeof(init));
  init.size = sizeof(init);
  init.horizontal = DEC_MAX_WIDTH;
  init.vertical = DEC_MAX_HEIGHT;
  init.numOfRefFrames = NUM_REF_FRAMES;
  init.numOfStreams = 1;
  ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
  if (ret < 0) {
    printf("decoder: sceVideodecInitLibrary 0x%08x\n", ret);
    goto fail;
  }
  D.lib_inited = 1;
  /* The decoder otherwise holds ~4 pictures before the first output, which
   * freezes a mostly static page (the backend sends frames only on change). */
  /* sceAvcdecSetLowDelayModeNongameapp crashes a game-type homebrew on 3.74
   * (tested 12-oct): not called. */
  dec_lowdelay_ret = 0;

  SceAvcdecQueryDecoderInfo q;
  SceAvcdecDecoderInfo qi;
  memset(&q, 0, sizeof(q));
  memset(&qi, 0, sizeof(qi));
  q.horizontal = init.horizontal;
  q.vertical = init.vertical;
  q.numOfRefFrames = init.numOfRefFrames;
  ret = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &qi);
  if (ret < 0) {
    printf("decoder: sceAvcdecQueryDecoderMemSize 0x%08x\n", ret);
    goto fail;
  }

  /* Decoder working memory: physically contiguous, size rounded to 1 MiB
   * (the PHYCONT memblock granularity). */
  SceSize sz = ALIGN_UP(qi.frameMemSize, 1024 * 1024);
  D.frame_block = sceKernelAllocMemBlock("sv_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
  if (D.frame_block < 0) {
    ret = D.frame_block;
    printf("decoder: alloc phycont 0x%x bytes: 0x%08x\n", (unsigned)sz, ret);
    D.frame_block = -1;
    goto fail;
  }
  void *base = NULL;
  ret = sceKernelGetMemBlockBase(D.frame_block, &base);
  if (ret < 0) goto fail;

  memset(&D.ctrl, 0, sizeof(D.ctrl));
  D.ctrl.frameBuf.pBuf = base;
  D.ctrl.frameBuf.size = sz;
  ret = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &D.ctrl, &q);
  if (ret < 0) {
    printf("decoder: sceAvcdecCreateDecoder 0x%08x\n", ret);
    goto fail;
  }
  D.ctrl_created = 1;

  pthread_mutex_init(&D.lock, NULL);
  pthread_cond_init(&D.cond, NULL);
  D.initialized = 1;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64 * 1024);
  ret = pthread_create(&D.thread, &attr, decoder_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0) {
    printf("decoder: pthread_create %d\n", ret);
    D.initialized = 0;
    ret = -1;
    goto fail;
  }
  D.thread_started = 1;
  return 0;

fail:
  dec_shutdown();
  return ret;
}

void dec_shutdown(void) {
  if (D.thread_started) {
    pthread_mutex_lock(&D.lock);
    D.quit = 1;
    pthread_cond_signal(&D.cond);
    pthread_mutex_unlock(&D.lock);
    pthread_join(D.thread, NULL);
    D.thread_started = 0;
  }
  if (D.initialized) {
    pthread_mutex_lock(&D.lock);
    queue_clear_locked();
    pthread_mutex_unlock(&D.lock);
    pthread_mutex_destroy(&D.lock);
    pthread_cond_destroy(&D.cond);
    D.initialized = 0;
  }
  if (D.ctrl_created) {
    sceAvcdecDeleteDecoder(&D.ctrl);
    D.ctrl_created = 0;
  }
  if (D.frame_block >= 0) {
    sceKernelFreeMemBlock(D.frame_block);
    D.frame_block = -1;
  }
  if (D.lib_inited) {
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    D.lib_inited = 0;
  }
  vita2d_wait_rendering_done();
  for (int i = 0; i < NUM_TEXTURES; i++) {
    if (D.slots[i].tex) {
      vita2d_free_texture(D.slots[i].tex);
      D.slots[i].tex = NULL;
    }
  }
  if (D.sysmodule_loaded) {
    sceSysmoduleUnloadModule(SCE_SYSMODULE_AVCDEC);
    D.sysmodule_loaded = 0;
  }
}
