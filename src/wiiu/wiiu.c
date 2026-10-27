#include "wiiu.h"

#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <stdarg.h>

#include <whb/proc.h>
#include <whb/gfx.h>
#include <coreinit/fastmutex.h>
#include <gx2/mem.h>
#include <gx2/draw.h>
#include <gx2/registers.h>

#include "shaders/display.h"

#define ATTRIB_SIZE (8 * 2 * sizeof(float))
#define ATTRIB_STRIDE (4 * sizeof(float))

static GX2Sampler screenSamp;
static WHBGfxShaderGroup shaderGroup;

static float* tvAttribs;
static float* drcAttribs;

static float tvScreenSize[2];
static float drcScreenSize[2];

int dualscreen = 0;
screen_region_t tv_region = {0.0f, 0.0f, 1.0f, 0.5f};
screen_region_t drc_region = {0.0f, 0.5f, 1.0f, 1.0f};

int stream_width = 1280;
int stream_height = 720;
float drc_touch_x = 0.0f;
float drc_touch_y = 0.0f;
float drc_touch_w = (float) TOUCH_WIDTH;
float drc_touch_h = (float) TOUCH_HEIGHT;

uint32_t currentFrame;
uint32_t nextFrame;

static OSFastMutex queueMutex;
static yuv_texture_t* queueMessages[MAX_QUEUEMESSAGES];
static uint32_t queueWriteIndex;
static uint32_t queueReadIndex;

// Matches H264_FRAME_HEIGHT() in decode.c. The Y texture is that tall, so a
// 0..1 texcoord covers padding below the picture. Crops are in picture space.
static uint32_t aligned_frame_height(uint32_t height)
{
  return (height + 0xfu) & ~0xfu;
}

static void write_quad(float* attribs, float x, float y, float w, float h,
                        float u0, float v0, float u1, float v1)
{
  int i = 0;

  attribs[i++] = x;         attribs[i++] = y;
  attribs[i++] = u0;        attribs[i++] = v0;

  attribs[i++] = x + w;     attribs[i++] = y;
  attribs[i++] = u1;        attribs[i++] = v0;

  attribs[i++] = x + w;     attribs[i++] = y + h;
  attribs[i++] = u1;        attribs[i++] = v1;

  attribs[i++] = x;         attribs[i++] = y + h;
  attribs[i++] = u0;        attribs[i++] = v1;
}

// Fit the picture region inside the screen without stretching. UVs account for
// the decoder's padded texture height. out_x/y/w/h are in screen pixels.
static void place_region(float screen_w, float screen_h, const screen_region_t* region,
                          float* out_x, float* out_y, float* out_w, float* out_h,
                          float* u0, float* v0, float* u1, float* v1)
{
  float left = region->left;
  float top = region->top;
  float right = region->right;
  float bottom = region->bottom;
  float region_w;
  float region_h;
  float scale;
  float scale_h;
  uint32_t aligned_h;
  float v_scale;

  if (right < left) {
    float swap = left;
    left = right;
    right = swap;
  }
  if (bottom < top) {
    float swap = top;
    top = bottom;
    bottom = swap;
  }

  region_w = (right - left) * (float) stream_width;
  region_h = (bottom - top) * (float) stream_height;
  if (region_w < 1.0f)
    region_w = 1.0f;
  if (region_h < 1.0f)
    region_h = 1.0f;

  scale = screen_w / region_w;
  scale_h = screen_h / region_h;
  if (scale_h < scale)
    scale = scale_h;

  *out_w = region_w * scale;
  *out_h = region_h * scale;
  *out_x = (screen_w - *out_w) * 0.5f;
  *out_y = (screen_h - *out_h) * 0.5f;

  aligned_h = aligned_frame_height((uint32_t) stream_height);
  v_scale = (float) stream_height / (float) aligned_h;
  *u0 = left;
  *u1 = right;
  *v0 = top * v_scale;
  *v1 = bottom * v_scale;
}

void wiiu_stream_init(uint32_t width, uint32_t height)
{
  currentFrame = nextFrame = 0;

  OSFastMutex_Init(&queueMutex, "");
  queueReadIndex = queueWriteIndex = 0;

  if (!WHBGfxLoadGFDShaderGroup(&shaderGroup, 0, display_gsh)) {
    printf("Cannot load shader\n");
  }

  WHBGfxInitShaderAttribute(&shaderGroup, "in_pos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
  WHBGfxInitShaderAttribute(&shaderGroup, "in_texCoord", 0, 8, GX2_ATTRIB_FORMAT_FLOAT_32_32);

  if (!WHBGfxInitFetchShader(&shaderGroup)) {
    printf("cannot init shader\n");
  }

  GX2InitSampler(&screenSamp,
    dualscreen ? GX2_TEX_CLAMP_MODE_CLAMP : GX2_TEX_CLAMP_MODE_WRAP,
    GX2_TEX_XY_FILTER_MODE_POINT);

  if (width > 0 && height > 0) {
    stream_width = (int) width;
    stream_height = (int) height;
  }

  GX2ColorBuffer* cb = WHBGfxGetTVColourBuffer();
  tvScreenSize[0] = 1.0f / (float) cb->surface.width;
  tvScreenSize[1] = 1.0f / (float) cb->surface.height;

  tvAttribs = memalign(GX2_VERTEX_BUFFER_ALIGNMENT, ATTRIB_SIZE);
  if (dualscreen) {
    float x, y, w, h, u0, v0, u1, v1;
    place_region((float) cb->surface.width, (float) cb->surface.height, &tv_region,
                 &x, &y, &w, &h, &u0, &v0, &u1, &v1);
    write_quad(tvAttribs, x, y, w, h, u0, v0, u1, v1);
  }
  else {
    write_quad(tvAttribs, 0.0f, 0.0f, (float) cb->surface.width, (float) cb->surface.height,
               0.0f, 0.0f, 1.0f, 1.0f);
  }
  GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, tvAttribs, ATTRIB_SIZE);

  cb = WHBGfxGetDRCColourBuffer();
  drcScreenSize[0] = 1.0f / (float) cb->surface.width;
  drcScreenSize[1] = 1.0f / (float) cb->surface.height;

  drcAttribs = memalign(GX2_VERTEX_BUFFER_ALIGNMENT, ATTRIB_SIZE);
  if (dualscreen) {
    float x, y, w, h, u0, v0, u1, v1;
    place_region((float) cb->surface.width, (float) cb->surface.height, &drc_region,
                 &x, &y, &w, &h, &u0, &v0, &u1, &v1);
    write_quad(drcAttribs, x, y, w, h, u0, v0, u1, v1);

    // Same quad, expressed in the 1280x720 touch plane, so the stylus matches the picture.
    drc_touch_x = x * ((float) TOUCH_WIDTH / (float) cb->surface.width);
    drc_touch_y = y * ((float) TOUCH_HEIGHT / (float) cb->surface.height);
    drc_touch_w = w * ((float) TOUCH_WIDTH / (float) cb->surface.width);
    drc_touch_h = h * ((float) TOUCH_HEIGHT / (float) cb->surface.height);
  }
  else {
    write_quad(drcAttribs, 0.0f, 0.0f, (float) cb->surface.width, (float) cb->surface.height,
               0.0f, 0.0f, 1.0f, 1.0f);
    drc_touch_x = 0.0f;
    drc_touch_y = 0.0f;
    drc_touch_w = (float) TOUCH_WIDTH;
    drc_touch_h = (float) TOUCH_HEIGHT;
  }
  GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, drcAttribs, ATTRIB_SIZE);
}

void wiiu_stream_draw(void)
{
  yuv_texture_t* tex = get_frame();
  if (tex) {
    if (++currentFrame <= nextFrame - NUM_BUFFERS) {
      // display thread is behind decoder, skip frame
    }
    else {
      WHBGfxBeginRender();

      // TV
      WHBGfxBeginRenderTV();
      WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);

      GX2SetPixelTexture(&tex->yTex, 0);
      GX2SetPixelTexture(&tex->uvTex, 1);
      GX2SetPixelSampler(&screenSamp, 0);
      GX2SetPixelSampler(&screenSamp, 1);

      GX2SetFetchShader(&shaderGroup.fetchShader);
      GX2SetVertexShader(shaderGroup.vertexShader);
      GX2SetPixelShader(shaderGroup.pixelShader);

      GX2SetVertexUniformReg(0, 2, tvScreenSize);
      GX2SetAttribBuffer(0, ATTRIB_SIZE, ATTRIB_STRIDE, tvAttribs);
      GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);

      WHBGfxFinishRenderTV();

      // DRC
      WHBGfxBeginRenderDRC();
      WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);

      GX2SetPixelTexture(&tex->yTex, 0);
      GX2SetPixelTexture(&tex->uvTex, 1);
      GX2SetPixelSampler(&screenSamp, 0);
      GX2SetPixelSampler(&screenSamp, 1);

      GX2SetFetchShader(&shaderGroup.fetchShader);
      GX2SetVertexShader(shaderGroup.vertexShader);
      GX2SetPixelShader(shaderGroup.pixelShader);

      GX2SetVertexUniformReg(0, 2, drcScreenSize);
      GX2SetAttribBuffer(0, ATTRIB_SIZE, ATTRIB_STRIDE, drcAttribs);
      GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
      
      WHBGfxFinishRenderDRC();

      WHBGfxFinishRender();
    }
  }
}

void wiiu_stream_fini(void)
{
  free(tvAttribs);
  free(drcAttribs);

  WHBGfxFreeShaderGroup(&shaderGroup);
}

void* get_frame(void)
{
  OSFastMutex_Lock(&queueMutex);

  uint32_t elements_in = queueWriteIndex - queueReadIndex;
  if(elements_in == 0) {
    OSFastMutex_Unlock(&queueMutex);
    return NULL; // framequeue is empty
  }

  uint32_t i = (queueReadIndex)++ & (MAX_QUEUEMESSAGES - 1);
  yuv_texture_t* message = queueMessages[i];

  OSFastMutex_Unlock(&queueMutex);
  return message;
}

void add_frame(yuv_texture_t* msg)
{
  OSFastMutex_Lock(&queueMutex);

  uint32_t elements_in = queueWriteIndex - queueReadIndex;
  if (elements_in == MAX_QUEUEMESSAGES) {
    OSFastMutex_Unlock(&queueMutex);
    return; // framequeue is full
  }

  uint32_t i = (queueWriteIndex)++ & (MAX_QUEUEMESSAGES - 1);
  queueMessages[i] = msg;

  OSFastMutex_Unlock(&queueMutex);
}

void wiiu_setup_renderstate(void)
{
  WHBGfxBeginRenderTV();
  GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
  GX2SetBlendControl(GX2_RENDER_TARGET_0,
    GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_COMBINE_MODE_ADD,
    TRUE,
    GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_COMBINE_MODE_ADD
  );
  GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
  WHBGfxBeginRenderDRC();
  GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
  GX2SetBlendControl(GX2_RENDER_TARGET_0,
    GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_COMBINE_MODE_ADD,
    TRUE,
    GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_COMBINE_MODE_ADD
  );
  GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
}
