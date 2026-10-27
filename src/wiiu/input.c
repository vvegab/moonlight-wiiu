#include "../config.h"
#include "wiiu.h"

#include <malloc.h>

#include <vpad/input.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <coreinit/time.h>
#include <coreinit/alarm.h>
#include <coreinit/thread.h>

#define millis() OSTicksToMilliseconds(OSGetTime())

int disable_gamepad = 0;
int swap_buttons = 0;
mouse_modes mouse_mode = MOUSE_MODE_RELATIVE;
int citra_input = 0;

static char lastTouched = 0;
static char touched = 0;

static uint16_t last_x = 0;
static uint16_t last_y = 0;

#define TAP_MILLIS 100
#define DRAG_DISTANCE 10
static uint64_t touchDownMillis = 0;

// Identical gamepad reports are reliable ENet packets. At 60 Hz they crowd
// out ACKs on the Wii U's Wi-Fi and the host drops the stream. Send at once
// when the pad actually moves, and otherwise keep a 10 Hz heartbeat.
#define PAD_HEARTBEAT_MS 100
#define STICK_DELTA 400

typedef struct {
  int buttonFlags;
  unsigned char leftTrigger;
  unsigned char rightTrigger;
  short lx, ly, rx, ry;
  uint64_t lastSent;
  int valid;
} pad_report_t;

static pad_report_t pad_reports[4];

static int stick_moved(short a, short b) {
  int d = (int) a - (int) b;
  if (d < 0) d = -d;
  return d > STICK_DELTA;
}

static int pad_report_due(int index, int buttonFlags,
    unsigned char leftTrigger, unsigned char rightTrigger,
    short lx, short ly, short rx, short ry) {
  pad_report_t* s;
  int changed;

  if (index < 0 || index >= 4)
    return 1;

  s = &pad_reports[index];
  changed = !s->valid ||
    s->buttonFlags != buttonFlags ||
    s->leftTrigger != leftTrigger ||
    s->rightTrigger != rightTrigger ||
    stick_moved(s->lx, lx) || stick_moved(s->ly, ly) ||
    stick_moved(s->rx, rx) || stick_moved(s->ry, ry);
  if (!changed && millis() - s->lastSent < PAD_HEARTBEAT_MS)
    return 0;

  s->valid = 1;
  s->buttonFlags = buttonFlags;
  s->leftTrigger = leftTrigger;
  s->rightTrigger = rightTrigger;
  s->lx = lx;
  s->ly = ly;
  s->rx = rx;
  s->ry = ry;
  s->lastSent = millis();
  return 1;
}

// Citra's default profile listens to a keyboard, not to the virtual Xbox pad
// Sunshine creates. These are the Win32 VK codes for that profile:
// A/B/X/Y, d-pad, L/R, ZL/ZR, Start/Select, circle pad, C-stick.
static const short citra_keys[] = {
  0x41, 0x53, 0x5A, 0x58,
  0x54, 0x47, 0x46, 0x48,
  0x51, 0x57, 0x31, 0x32,
  0x4D, 0x4E,
  0x25, 0x26, 0x27, 0x28,
  0x49, 0x4A, 0x4B, 0x4C
};
#define CITRA_KEY_COUNT (sizeof(citra_keys) / sizeof(citra_keys[0]))
static unsigned char citra_key_down[CITRA_KEY_COUNT];

static void citra_set(int index, int down) {
  unsigned char want = down ? 1 : 0;
  if (index < 0 || index >= (int) CITRA_KEY_COUNT)
    return;
  if (citra_key_down[index] == want)
    return;
  citra_key_down[index] = want;
  LiSendKeyboardEvent(citra_keys[index], want ? KEY_ACTION_DOWN : KEY_ACTION_UP, 0);
}

static void citra_release_all(void) {
  for (int i = 0; i < (int) CITRA_KEY_COUNT; i++)
    citra_set(i, 0);
}

static void citra_from_vpad(const VPADStatus* vpad) {
  uint32_t b = vpad->hold;
  float lx = vpad->leftStick.x;
  float ly = vpad->leftStick.y;
  float rx = vpad->rightStick.x;
  float ry = vpad->rightStick.y;
  const float t = 0.45f;

  citra_set(0, b & VPAD_BUTTON_A);
  citra_set(1, b & VPAD_BUTTON_B);
  citra_set(2, b & VPAD_BUTTON_X);
  citra_set(3, b & VPAD_BUTTON_Y);
  citra_set(4, b & VPAD_BUTTON_UP);
  citra_set(5, b & VPAD_BUTTON_DOWN);
  citra_set(6, b & VPAD_BUTTON_LEFT);
  citra_set(7, b & VPAD_BUTTON_RIGHT);
  citra_set(8, b & VPAD_BUTTON_L);
  citra_set(9, b & VPAD_BUTTON_R);
  citra_set(10, b & VPAD_BUTTON_ZL);
  citra_set(11, b & VPAD_BUTTON_ZR);
  citra_set(12, b & VPAD_BUTTON_PLUS);
  citra_set(13, b & VPAD_BUTTON_MINUS);
  citra_set(14, lx < -t);
  citra_set(15, ly > t);
  citra_set(16, lx > t);
  citra_set(17, ly < -t);
  citra_set(18, ry > t);
  citra_set(19, rx < -t);
  citra_set(20, ry < -t);
  citra_set(21, rx > t);
}

static int thread_running;
static OSThread inputThread;
static OSAlarm inputAlarm;

// ~60 Hz
#define INPUT_UPDATE_RATE OSMillisecondsToTicks(16)

// Map a GamePad touch into drc_region. The content quad is letterboxed the same
// way it is drawn. A new touch on the bars is ignored; an ongoing drag is clamped
// so the stroke does not jump off the bottom screen.
static int map_dual_touch(uint16_t tx, uint16_t ty, int allow_clamp, float* nx, float* ny) {
  float x = (float) tx;
  float y = (float) ty;
  float left = drc_touch_x;
  float top = drc_touch_y;
  float right = left + drc_touch_w;
  float bottom = top + drc_touch_h;
  int inside = (x >= left && x <= right && y >= top && y <= bottom);
  float u, v, rw, rh;

  if (!inside && !allow_clamp)
    return 0;
  if (drc_touch_w <= 0.0f || drc_touch_h <= 0.0f || stream_width <= 0 || stream_height <= 0)
    return 0;

  if (x < left) x = left;
  if (x > right) x = right;
  if (y < top) y = top;
  if (y > bottom) y = bottom;

  u = (x - left) / drc_touch_w;
  v = (y - top) / drc_touch_h;
  rw = drc_region.right - drc_region.left;
  rh = drc_region.bottom - drc_region.top;
  *nx = drc_region.left + u * rw;
  *ny = drc_region.top + v * rh;
  if (*nx < 0.0f) *nx = 0.0f;
  if (*ny < 0.0f) *ny = 0.0f;
  if (*nx > 1.0f) *nx = 1.0f;
  if (*ny > 1.0f) *ny = 1.0f;
  return 1;
}

static float last_dual_x = 0.0f;
static float last_dual_y = 0.0f;

static void send_dual_mouse(float nx, float ny) {
  short sx = (short) (nx * (float) stream_width);
  short sy = (short) (ny * (float) stream_height);
  if (sx < 0) sx = 0;
  if (sy < 0) sy = 0;
  if (sx >= stream_width) sx = (short) (stream_width - 1);
  if (sy >= stream_height) sy = (short) (stream_height - 1);
  LiSendMousePositionEvent(sx, sy, (short) stream_width, (short) stream_height);
}

void handleTouch(VPADTouchData touch) {
  if (mouse_mode == MOUSE_MODE_ABSOLUTE) {
    if (dualscreen) {
      float nx, ny;
      if (touch.touched) {
        if (!map_dual_touch(touch.x, touch.y, touched, &nx, &ny))
          return;
        send_dual_mouse(nx, ny);
        if (!touched) {
          LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
          touched = 1;
        }
      }
      else if (touched) {
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        touched = 0;
      }
    }
    else if (touch.touched) {
      LiSendMousePositionEvent(touch.x, touch.y, TOUCH_WIDTH, TOUCH_HEIGHT);

      if (!touched) {
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        touched = 1;
      }
    }
    else if (touched) {
      LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
      touched = 0;
    }
  }
  else if (mouse_mode == MOUSE_MODE_TOUCHSCREEN) {
    if (dualscreen) {
      float nx, ny;
      if (touch.touched) {
        if (!map_dual_touch(touch.x, touch.y, touched, &nx, &ny))
          return;
        last_dual_x = nx;
        last_dual_y = ny;
        if (!touched) {
          LiSendTouchEvent(LI_TOUCH_EVENT_DOWN, 0, nx, ny, 0.0, 0.0, 0.0, 0.0);
          touched = 1;
        } else {
          LiSendTouchEvent(LI_TOUCH_EVENT_MOVE, 0, nx, ny, 0.0, 0.0, 0.0, 0.0);
        }
      }
      else if (touched) {
        LiSendTouchEvent(LI_TOUCH_EVENT_UP, 0, last_dual_x, last_dual_y, 0.0, 0.0, 0.0, 0.0);
        touched = 0;
      }
    }
    else if (touch.touched) {
      if (!touched) {
        LiSendTouchEvent(LI_TOUCH_EVENT_DOWN, 0, (float) touch.x / TOUCH_WIDTH, (float) touch.y / TOUCH_HEIGHT, 0.0, 0.0, 0.0, 0.0);
        touched = 1;
      } else {
        LiSendTouchEvent(LI_TOUCH_EVENT_MOVE, 0, (float) touch.x / TOUCH_WIDTH, (float) touch.y / TOUCH_HEIGHT, 0.0, 0.0, 0.0, 0.0);
      }
    }
    else if (touched) {
      LiSendTouchEvent(LI_TOUCH_EVENT_UP, 0, (float) touch.x / TOUCH_WIDTH, (float) touch.y / TOUCH_HEIGHT, 0.0, 0.0, 0.0, 0.0);
      touched = 0;
    }
  }
  else {
    // Just pressed (run this twice to allow touch position to settle)
    if (lastTouched < 2 && touch.touched) {
      touchDownMillis = millis();
      last_x = touch.x;
      last_y = touch.y;

      lastTouched++;
      return; // We can't do much until we wait for a few hundred milliseconds
              // since we don't know if it's a tap, a tap-and-hold, or a drag
    }

    // Just released
    if (lastTouched && !touch.touched) {
      if (millis() - touchDownMillis < TAP_MILLIS) {
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
      }
    }

    if (touch.touched) {
      // Holding & dragging screen, not just tapping
      if (millis() - touchDownMillis > TAP_MILLIS || touchDownMillis == 0) {
        if (touch.x != last_x || touch.y != last_y) // Don't send extra data if we don't need to
          LiSendMouseMoveEvent(touch.x - last_x, touch.y - last_y);
        last_x = touch.x;
        last_y = touch.y;
      } else {
        if (touch.x - last_x < -10 || touch.x - last_x > 10) touchDownMillis=0;
        if (touch.y - last_y < -10 || touch.y - last_y > 10) touchDownMillis=0;
        int16_t diff_x = touch.x - last_x;
        int16_t diff_y = touch.y - last_y;
        if (diff_x < 0) diff_x = -diff_x;
        if (diff_y < 0) diff_y = -diff_y;
        if (diff_x + diff_y > DRAG_DISTANCE) touchDownMillis = 0;
      }
    }

    lastTouched = touch.touched ? lastTouched : 0; // Keep value unless released
  }
}

void wiiu_input_init(void)
{
	KPADInit();
	WPADEnableURCC(1);
}

void wiiu_input_update(void) {
  static uint64_t home_pressed[4] = {0};

  short controllerNumber = 0;
  short gamepad_mask = 0;

  for (int i = 0; i < wiiu_input_num_controllers(); i++)
    gamepad_mask |= 1 << i;

  VPADStatus vpad;
  VPADReadError err;
  VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
  if (err == VPAD_READ_SUCCESS && !disable_gamepad) {
    uint32_t btns = vpad.hold;
    short buttonFlags = 0;
#define CHECKBTN(v, f) if (btns & v) buttonFlags |= f;
    if (swap_buttons) {
      CHECKBTN(VPAD_BUTTON_A,       B_FLAG);
      CHECKBTN(VPAD_BUTTON_B,       A_FLAG);
      CHECKBTN(VPAD_BUTTON_X,       Y_FLAG);
      CHECKBTN(VPAD_BUTTON_Y,       X_FLAG);
    }
    else {
      CHECKBTN(VPAD_BUTTON_A,       A_FLAG);
      CHECKBTN(VPAD_BUTTON_B,       B_FLAG);
      CHECKBTN(VPAD_BUTTON_X,       X_FLAG);
      CHECKBTN(VPAD_BUTTON_Y,       Y_FLAG);
    }
    CHECKBTN(VPAD_BUTTON_UP,      UP_FLAG);
    CHECKBTN(VPAD_BUTTON_DOWN,    DOWN_FLAG);
    CHECKBTN(VPAD_BUTTON_LEFT,    LEFT_FLAG);
    CHECKBTN(VPAD_BUTTON_RIGHT,   RIGHT_FLAG);
    CHECKBTN(VPAD_BUTTON_L,       LB_FLAG);
    CHECKBTN(VPAD_BUTTON_R,       RB_FLAG);
    CHECKBTN(VPAD_BUTTON_STICK_L, LS_CLK_FLAG);
    CHECKBTN(VPAD_BUTTON_STICK_R, RS_CLK_FLAG);
    CHECKBTN(VPAD_BUTTON_PLUS,    PLAY_FLAG);
    CHECKBTN(VPAD_BUTTON_MINUS,   BACK_FLAG);
    CHECKBTN(VPAD_BUTTON_HOME,    SPECIAL_FLAG);
#undef CHECKBTN

    // If the button was just pressed, reset to current time
    if (vpad.trigger & VPAD_BUTTON_HOME) home_pressed[controllerNumber] = millis();

    if (btns & VPAD_BUTTON_HOME && millis() - home_pressed[controllerNumber] > 3000) {
      state = STATE_STOP_STREAM;
      return;
    }

    {
      unsigned char lt = (vpad.hold & VPAD_BUTTON_ZL) ? 0xFF : 0;
      unsigned char rt = (vpad.hold & VPAD_BUTTON_ZR) ? 0xFF : 0;
      short lx = (short) (vpad.leftStick.x * INT16_MAX);
      short ly = (short) (vpad.leftStick.y * INT16_MAX);
      short rx = (short) (vpad.rightStick.x * INT16_MAX);
      short ry = (short) (vpad.rightStick.y * INT16_MAX);
      if (pad_report_due(controllerNumber, buttonFlags, lt, rt, lx, ly, rx, ry))
        LiSendMultiControllerEvent(controllerNumber, gamepad_mask, buttonFlags, lt, rt, lx, ly, rx, ry);
      controllerNumber++;
    }

    if (citra_input)
      citra_from_vpad(&vpad);

    VPADTouchData touch;
    VPADGetTPCalibratedPoint(VPAD_CHAN_0, &touch, &vpad.tpNormal);
    handleTouch(touch);
  }

  KPADStatus kpad_data = {0};
	int32_t kpad_err = -1;
	for (int i = 0; i < 4; i++) {
		KPADReadEx((KPADChan) i, &kpad_data, 1, &kpad_err);
		if (kpad_err == KPAD_ERROR_OK && controllerNumber < 4) {
      if (kpad_data.extensionType == WPAD_EXT_PRO_CONTROLLER) {
        uint32_t btns = kpad_data.pro.hold;
        short buttonFlags = 0;
#define CHECKBTN(v, f) if (btns & v) buttonFlags |= f;
        if (swap_buttons) {
          CHECKBTN(WPAD_PRO_BUTTON_A,       B_FLAG);
          CHECKBTN(WPAD_PRO_BUTTON_B,       A_FLAG);
          CHECKBTN(WPAD_PRO_BUTTON_X,       Y_FLAG);
          CHECKBTN(WPAD_PRO_BUTTON_Y,       X_FLAG);
        }
        else {
          CHECKBTN(WPAD_PRO_BUTTON_A,       A_FLAG);
          CHECKBTN(WPAD_PRO_BUTTON_B,       B_FLAG);
          CHECKBTN(WPAD_PRO_BUTTON_X,       X_FLAG);
          CHECKBTN(WPAD_PRO_BUTTON_Y,       Y_FLAG);
        }
        CHECKBTN(WPAD_PRO_BUTTON_UP,      UP_FLAG);
        CHECKBTN(WPAD_PRO_BUTTON_DOWN,    DOWN_FLAG);
        CHECKBTN(WPAD_PRO_BUTTON_LEFT,    LEFT_FLAG);
        CHECKBTN(WPAD_PRO_BUTTON_RIGHT,   RIGHT_FLAG);
        CHECKBTN(WPAD_PRO_TRIGGER_L,      LB_FLAG);
        CHECKBTN(WPAD_PRO_TRIGGER_R,      RB_FLAG);
        CHECKBTN(WPAD_PRO_BUTTON_STICK_L, LS_CLK_FLAG);
        CHECKBTN(WPAD_PRO_BUTTON_STICK_R, RS_CLK_FLAG);
        CHECKBTN(WPAD_PRO_BUTTON_PLUS,    PLAY_FLAG);
        CHECKBTN(WPAD_PRO_BUTTON_MINUS,   BACK_FLAG);
        CHECKBTN(WPAD_PRO_BUTTON_HOME,    SPECIAL_FLAG);
#undef CHECKBTN

        // If the button was just pressed, reset to current time
        if (kpad_data.pro.trigger & WPAD_PRO_BUTTON_HOME)
          home_pressed[controllerNumber] = millis();

        if (btns & WPAD_PRO_BUTTON_HOME && millis() - home_pressed[controllerNumber] > 3000) {
          state = STATE_STOP_STREAM;
          return;
        }

        {
          unsigned char lt = (kpad_data.pro.hold & WPAD_PRO_TRIGGER_ZL) ? 0xFF : 0;
          unsigned char rt = (kpad_data.pro.hold & WPAD_PRO_TRIGGER_ZR) ? 0xFF : 0;
          short lx = (short) (kpad_data.pro.leftStick.x * INT16_MAX);
          short ly = (short) (kpad_data.pro.leftStick.y * INT16_MAX);
          short rx = (short) (kpad_data.pro.rightStick.x * INT16_MAX);
          short ry = (short) (kpad_data.pro.rightStick.y * INT16_MAX);
          if (pad_report_due(controllerNumber, buttonFlags, lt, rt, lx, ly, rx, ry))
            LiSendMultiControllerEvent(controllerNumber, gamepad_mask, buttonFlags, lt, rt, lx, ly, rx, ry);
          controllerNumber++;
        }
      }
      else if (kpad_data.extensionType == WPAD_EXT_CLASSIC || kpad_data.extensionType == WPAD_EXT_MPLUS_CLASSIC) {
        uint32_t btns = kpad_data.classic.hold;
        short buttonFlags = 0;
#define CHECKBTN(v, f) if (btns & v) buttonFlags |= f;
        if (swap_buttons) {
          CHECKBTN(WPAD_CLASSIC_BUTTON_A,       B_FLAG);
          CHECKBTN(WPAD_CLASSIC_BUTTON_B,       A_FLAG);
          CHECKBTN(WPAD_CLASSIC_BUTTON_X,       Y_FLAG);
          CHECKBTN(WPAD_CLASSIC_BUTTON_Y,       X_FLAG);
        }
        else {
          CHECKBTN(WPAD_CLASSIC_BUTTON_A,       A_FLAG);
          CHECKBTN(WPAD_CLASSIC_BUTTON_B,       B_FLAG);
          CHECKBTN(WPAD_CLASSIC_BUTTON_X,       X_FLAG);
          CHECKBTN(WPAD_CLASSIC_BUTTON_Y,       Y_FLAG);
        }
        CHECKBTN(WPAD_CLASSIC_BUTTON_UP,      UP_FLAG);
        CHECKBTN(WPAD_CLASSIC_BUTTON_DOWN,    DOWN_FLAG);
        CHECKBTN(WPAD_CLASSIC_BUTTON_LEFT,    LEFT_FLAG);
        CHECKBTN(WPAD_CLASSIC_BUTTON_RIGHT,   RIGHT_FLAG);
        CHECKBTN(WPAD_CLASSIC_BUTTON_L,       LB_FLAG);
        CHECKBTN(WPAD_CLASSIC_BUTTON_R,       RB_FLAG);
        // don't have stick buttons on a classic controller
        // CHECKBTN(WPAD_CLASSIC_BUTTON_STICK_L, LS_CLK_FLAG);
        // CHECKBTN(WPAD_CLASSIC_BUTTON_STICK_R, RS_CLK_FLAG);
        CHECKBTN(WPAD_CLASSIC_BUTTON_PLUS,    PLAY_FLAG);
        CHECKBTN(WPAD_CLASSIC_BUTTON_MINUS,   BACK_FLAG);
        CHECKBTN(WPAD_CLASSIC_BUTTON_HOME,    SPECIAL_FLAG);
#undef CHECKBTN

        // If the button was just pressed, reset to current time
        if (kpad_data.classic.trigger & WPAD_CLASSIC_BUTTON_HOME)
          home_pressed[controllerNumber] = millis();

        if (btns & WPAD_CLASSIC_BUTTON_HOME && millis() - home_pressed[controllerNumber] > 3000) {
          state = STATE_STOP_STREAM;
          return;
        }

        {
          unsigned char lt = (kpad_data.classic.hold & WPAD_CLASSIC_BUTTON_ZL) ? 0xFF : 0x00;
          unsigned char rt = (kpad_data.classic.hold & WPAD_CLASSIC_BUTTON_ZR) ? 0xFF : 0x00;
          short lx = (short) (kpad_data.classic.leftStick.x * INT16_MAX);
          short ly = (short) (kpad_data.classic.leftStick.y * INT16_MAX);
          short rx = (short) (kpad_data.classic.rightStick.x * INT16_MAX);
          short ry = (short) (kpad_data.classic.rightStick.y * INT16_MAX);
          if (pad_report_due(controllerNumber, buttonFlags, lt, rt, lx, ly, rx, ry))
            LiSendMultiControllerEvent(controllerNumber, gamepad_mask, buttonFlags, lt, rt, lx, ly, rx, ry);
          controllerNumber++;
        }
      }
    }
  }
}

uint32_t wiiu_input_num_controllers(void)
{
  uint32_t numControllers = !disable_gamepad;

  WPADExtensionType type;
  for (int i = 0; i < 4; i++) {
    if (WPADProbe((WPADChan) i, &type) == 0) {
      if (type == WPAD_EXT_PRO_CONTROLLER || type == WPAD_EXT_CLASSIC || type == WPAD_EXT_MPLUS_CLASSIC) {
        numControllers++;
      }
    }
  }

  if (numControllers > 4) {
    numControllers = 4;
  }

  return numControllers;
}

uint32_t wiiu_input_buttons_triggered(void)
{
  uint32_t btns = 0;

  VPADStatus vpad;
  VPADReadError vpad_err;
  VPADRead(VPAD_CHAN_0, &vpad, 1, &vpad_err);
  if (vpad_err == VPAD_READ_SUCCESS) {
    btns |= vpad.trigger;
  }

  KPADStatus kpad_data = {0};
	int32_t kpad_err = -1;
	for (int i = 0; i < 4; i++) {
		KPADReadEx((KPADChan) i, &kpad_data, 1, &kpad_err);
		if (kpad_err == KPAD_ERROR_OK) {
      if (kpad_data.extensionType == WPAD_EXT_PRO_CONTROLLER) {
#define MAPBTNS(b, v) if (kpad_data.pro.trigger & b) btns |= v;
        MAPBTNS(WPAD_PRO_BUTTON_UP,       VPAD_BUTTON_UP);
        MAPBTNS(WPAD_PRO_BUTTON_LEFT,     VPAD_BUTTON_LEFT);
        MAPBTNS(WPAD_PRO_TRIGGER_ZR,      VPAD_BUTTON_ZR);
        MAPBTNS(WPAD_PRO_BUTTON_X,        VPAD_BUTTON_X);
        MAPBTNS(WPAD_PRO_BUTTON_A,        VPAD_BUTTON_A);
        MAPBTNS(WPAD_PRO_BUTTON_Y,        VPAD_BUTTON_Y);
        MAPBTNS(WPAD_PRO_BUTTON_B,        VPAD_BUTTON_B);
        MAPBTNS(WPAD_PRO_TRIGGER_ZL,      VPAD_BUTTON_ZL);
        MAPBTNS(WPAD_PRO_TRIGGER_R,       VPAD_BUTTON_R);
        MAPBTNS(WPAD_PRO_BUTTON_PLUS,     VPAD_BUTTON_PLUS);
        MAPBTNS(WPAD_PRO_BUTTON_HOME,     VPAD_BUTTON_HOME);
        MAPBTNS(WPAD_PRO_BUTTON_MINUS,    VPAD_BUTTON_MINUS);
        MAPBTNS(WPAD_PRO_TRIGGER_L,       VPAD_BUTTON_L);
        MAPBTNS(WPAD_PRO_BUTTON_DOWN,     VPAD_BUTTON_DOWN);
        MAPBTNS(WPAD_PRO_BUTTON_RIGHT,    VPAD_BUTTON_RIGHT);
        MAPBTNS(WPAD_PRO_BUTTON_STICK_R,  VPAD_BUTTON_STICK_R);
        MAPBTNS(WPAD_PRO_BUTTON_STICK_L,  VPAD_BUTTON_STICK_L);
#undef MAPBTNS
      }
      else if (kpad_data.extensionType == WPAD_EXT_CLASSIC || kpad_data.extensionType == WPAD_EXT_MPLUS_CLASSIC) {
#define MAPBTNS(b, v) if (kpad_data.classic.trigger & b) btns |= v;
        MAPBTNS(WPAD_CLASSIC_BUTTON_UP,     VPAD_BUTTON_UP);
        MAPBTNS(WPAD_CLASSIC_BUTTON_LEFT,   VPAD_BUTTON_LEFT);
        MAPBTNS(WPAD_CLASSIC_BUTTON_ZR,     VPAD_BUTTON_ZR);
        MAPBTNS(WPAD_CLASSIC_BUTTON_X,      VPAD_BUTTON_X);
        MAPBTNS(WPAD_CLASSIC_BUTTON_A,      VPAD_BUTTON_A);
        MAPBTNS(WPAD_CLASSIC_BUTTON_Y,      VPAD_BUTTON_Y);
        MAPBTNS(WPAD_CLASSIC_BUTTON_B,      VPAD_BUTTON_B);
        MAPBTNS(WPAD_CLASSIC_BUTTON_ZL,     VPAD_BUTTON_ZL);
        MAPBTNS(WPAD_CLASSIC_BUTTON_R,      VPAD_BUTTON_R);
        MAPBTNS(WPAD_CLASSIC_BUTTON_PLUS,   VPAD_BUTTON_PLUS);
        MAPBTNS(WPAD_CLASSIC_BUTTON_HOME,   VPAD_BUTTON_HOME);
        MAPBTNS(WPAD_CLASSIC_BUTTON_MINUS,  VPAD_BUTTON_MINUS);
        MAPBTNS(WPAD_CLASSIC_BUTTON_L,      VPAD_BUTTON_L);
        MAPBTNS(WPAD_CLASSIC_BUTTON_DOWN,   VPAD_BUTTON_DOWN);
        MAPBTNS(WPAD_CLASSIC_BUTTON_RIGHT,  VPAD_BUTTON_RIGHT);
#undef MAPBTNS
      }
      else {
        // meh we can't really map a wiimote to gamepad
      }
    }
  }

  return btns;
}

static void alarm_callback(OSAlarm* alarm, OSContext* ctx)
{
  wiiu_input_update();
}

static int input_thread_proc(int argc, const char **argv)
{
  OSCreateAlarm(&inputAlarm);
  OSSetPeriodicAlarm(&inputAlarm, 0, INPUT_UPDATE_RATE, alarm_callback);

  while (thread_running) {
    OSWaitAlarm(&inputAlarm);
  }
}

static void thread_deallocator(OSThread *thread, void *stack)
{
  free(stack);
}

void start_input_thread(void)
{
  const int stack_size = 4 * 1024 * 1024;
  uint8_t* stack = (uint8_t*)memalign(16, stack_size);
  if (!stack) {
    return;
  }

  if (!OSCreateThread(&inputThread,
                      input_thread_proc,
                      0, NULL,
                      stack + stack_size, stack_size,
                      0x10, OS_THREAD_ATTRIB_AFFINITY_ANY))
  {
    free(stack);
    return;
  }

  thread_running = 1;

  OSSetThreadName(&inputThread, "PadInput");
  OSSetThreadDeallocator(&inputThread, thread_deallocator);
  OSResumeThread(&inputThread);
}

void stop_input_thread(void)
{
  thread_running = 0;
  OSCancelAlarm(&inputAlarm);
  OSJoinThread(&inputThread, NULL);
  citra_release_all();
}
