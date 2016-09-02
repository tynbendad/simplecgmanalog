#include <pebble.h>
#include "pebble_chart.h"
#include "data-processor.h"
#include "cgm_info.h"

#define ANTIALIASING true
#define SNOOZE_KEY 1

#define BACKGROUND_COLOR bgcolor
#define FOREGROUND_COLOR fgcolor
#define COMP1_FGCOLOR cfgcolor
#define COMP1_BGCOLOR cbgcolor
#define COMP2_FGCOLOR cfgcolor
#define COMP2_BGCOLOR cbgcolor
#define COMP3_FGCOLOR cfgcolor
#define COMP3_BGCOLOR cbgcolor

static GColor8 curFG, curBG, fgcolor, bgcolor, cfgcolor, cbgcolor;

extern void analog_init(Layer *window_layer);
extern void analog_mode(GColor bg, GColor fg, int unused);
extern void analog_deinit();
extern void analog_window_load(Layer *window_layer);
extern void analog_window_unload();

typedef struct {
    int hours;
    int minutes;
} Time;

static Window * s_main_window;
static Layer * s_canvas_layer;

static AppTimer *timer, *timer2;

static GPoint s_center;
static Time s_last_time;
static int s_radius = 0, t_delta = 0, has_launched = 0, vibe_state = 1, alert_state = 0, check_count = 0, alert_snooze = 0;
static int * bgs;
static int * bg_times;
static int num_bgs = 0;
static int retry_interval = 5;
static int num_checks_throttle = 5;
static int tag_raw = 0;
static int is_web = 0;
    
static GBitmap *icon_bitmap = NULL;

static BitmapLayer * icon_layer;
static TextLayer * bg_layer, *delta_layer, *time_delta_layer, *iob_layer, *cob_layer, *time_layer, *date_layer, *batt_layer;

static char last_bg[124];
static int data_id = 99;
static char time_delta_str[124] = "";
static char bg_text[124] = "";
static char time_text[124] = "";
static char date_text[124] = "";
static char batt_text[124] = "";
static char iob_str[124] = "";
static char cob_str[124] = "";
static char snooze_str[124] = "";
static bool old_bg = false;

static int  tap_sec = 4;
static bool show_bg = true;
bool in_tap_handler = false;
bool showsec = false;

static ChartLayer* chart_layer;
static int trend_dir = 8;

enum CgmKey {
    CGM_EGV_DELTA_KEY = 0x0,
    CGM_EGV_KEY = 0x1,
    CGM_TREND_KEY = 0x2,
    CGM_ALERT_KEY = 0x3,
    CGM_VIBE_KEY = 0x4,
    CGM_ID = 0x5,
    CGM_TIME_DELTA_KEY = 0x6,
	CGM_BGS = 0x7,
    CGM_BG_TIMES = 0x8,
    CGM_IOB_KEY = 0x9,
    CGM_HIDE_KEY = 0xa,
    CGM_TAPTIME_KEY = 0xb,
    CGM_FGCOLOR_KEY = 0xc,
    CGM_BGCOLOR_KEY = 0xd,
    CGM_SHOWSEC_KEY = 0xe,
    CGM_COB_KEY = 0xf,
    CGM_CFGCOLOR_KEY = 0x10,
    CGM_CBGCOLOR_KEY = 0x11,
};


enum Alerts {
    OKAY = 0x0,
    LOSS_MID_NO_NOISE = 0x1,
    LOSS_HIGH_NO_NOISE = 0x2,
    NO_CHANGE = 0x3,
    OLD_DATA = 0x4,
};

static int s_color_channels[3] = { 85, 85, 85 };
static int b_color_channels[3] = { 0, 0, 0 };

static const uint32_t const error[] = { 100,100,100,100,100 };

#if 0 // OBSOLETE
static const uint32_t CGM_ICONS[] = {
    RESOURCE_ID_IMAGE_NONE_WHITE,	  //4 - 0
    RESOURCE_ID_IMAGE_UPUP_WHITE,     //0 - 1
    RESOURCE_ID_IMAGE_UP_WHITE,       //1 - 2
    RESOURCE_ID_IMAGE_UP45_WHITE,     //2 - 3
    RESOURCE_ID_IMAGE_FLAT_WHITE,     //3 - 4
    RESOURCE_ID_IMAGE_DOWN45_WHITE,   //5 - 5
    RESOURCE_ID_IMAGE_DOWN_WHITE,     //6 - 6
    RESOURCE_ID_IMAGE_DOWNDOWN_WHITE, //7 - 7
    RESOURCE_ID_IMAGE_REFRESH_WHITE   //    8
};
#endif

char *translate_error(AppMessageResult result) {
    switch (result) {
        case APP_MSG_OK: return "OK";
        case APP_MSG_SEND_TIMEOUT: return "ast";
        case APP_MSG_SEND_REJECTED: return "asr"; 
        case APP_MSG_NOT_CONNECTED: return "anc"; 
        case APP_MSG_APP_NOT_RUNNING: return "anr"; 
        case APP_MSG_INVALID_ARGS: return "aia"; 
        case APP_MSG_BUSY: return "aby"; 
        case APP_MSG_BUFFER_OVERFLOW: return "abo"; 
        case APP_MSG_ALREADY_RELEASED: return "aar";
        case APP_MSG_CALLBACK_ALREADY_REGISTERED: return "car"; 
        case APP_MSG_CALLBACK_NOT_REGISTERED: return "cnr";
        case APP_MSG_OUT_OF_MEMORY: return "oom";
        case APP_MSG_CLOSED: return "acd";
        case APP_MSG_INTERNAL_ERROR: return "aie";
        default: return "uer";
    }
}

static void update_hand_color() {
#if 0
    if ((s_color_channels[0] == 0) &&
        (s_color_channels[1] == 255) &&
        (s_color_channels[2] == 0)) {
        curBG = BACKGROUND_COLOR;
        curFG = FOREGROUND_COLOR;
    } else {
        curBG = GColorBlack;
        curFG = GColorYellow;
    }
#endif
    curBG = BACKGROUND_COLOR;
    curFG = FOREGROUND_COLOR;
    analog_mode(curBG, curFG, 0);
}

static bool is_snoozed() {
    time_t t = time(NULL);
    if (alert_snooze == -1) {
      // APP_LOG(APP_LOG_LEVEL_DEBUG, "Snooze Active (until cancelled)");
      return true;
    }
    if (t > alert_snooze) {
        // APP_LOG(APP_LOG_LEVEL_DEBUG, "Snooze Expired");
        return false;
    }
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Snooze Active");
    return true;
}

static void comm_alert() {
    
    VibePattern pat = {
        .durations = error,
        .num_segments = ARRAY_LENGTH(error),
    };     
    //APP_LOG(APP_LOG_LEVEL_INFO, "comm_alert: check_count=%d, t_delta=%d, vibe_state=%d, is_snoozed=%d", check_count, t_delta, vibe_state, is_snoozed());
    if ((check_count % 5 == 0 || t_delta % 5 == 0) && (vibe_state > 0) && !is_snoozed()) {
        vibes_enqueue_custom_pattern(pat);
    }
    
    b_color_channels[0] = 255;
    b_color_channels[1] = 0;
    b_color_channels[2] = 0;
    
    update_hand_color();
    
    layer_mark_dirty(s_canvas_layer);
}

/************************************ UI **************************************/
static void send_int(int key, int value) {
  DictionaryIterator *iter;
  AppMessageResult brslt = app_message_outbox_begin(&iter);
  DictionaryResult drslt = dict_write_int(iter, key, &value, sizeof(int), true);
  AppMessageResult srslt = app_message_outbox_send();
}
void send_cmd_connect() {
    data_id = 69;
    send_int(5, data_id);
} 

void send_cmd() {
    // APP_LOG(APP_LOG_LEVEL_INFO, "send_cmd");
    
    if (s_canvas_layer) {
        // gbitmap_destroy(icon_bitmap);
        snprintf(time_delta_str, 12, "chk(%d)", check_count++);

        if (check_count > 1)
            data_id = 99;

        if (check_count > 4)
            comm_alert();

        if(time_delta_layer)
            text_layer_set_text(time_delta_layer, time_delta_str);

        // icon_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_REFRESH_WHITE);
        // trend_dir = 8;
        // if(icon_layer)
        //    bitmap_layer_set_bitmap(icon_layer, icon_bitmap);
    }

    send_int(5, data_id);

    //APP_LOG(APP_LOG_LEVEL_INFO, "Message sent!");
    //APP_LOG(APP_LOG_LEVEL_INFO, "check_count: %d", check_count);
}

/*************Startup Timer*******/
//Message SHOULD come from smartphone app, but this will kick it off in less than 60 seconds if it can.
static void timer_callback(void *data) {   
    APP_LOG(APP_LOG_LEVEL_DEBUG, "send_cmd for timer_callback");
    send_cmd(); 
}

static void timer_callback_2(void *data) {   
    //send_cmd_connect();
    //timer2 = app_timer_register(60000*2, timer_callback_2, NULL);
}

static void clock_refresh(struct tm * tick_time) {
     char *time_format;
     char *date_format;
 
    if (!tick_time) {
        time_t now = time(NULL);
        tick_time = localtime(&now);
    }
    
#ifdef PBL_PLATFORM_CHALK 
    if (clock_is_24h_style()) {
        time_format = "%H:%M";
    } else {
        time_format = "%I:%M";
    }
    date_format = "%b%e";
#else
    if (clock_is_24h_style()) {
        time_format = "%H:%M";
    } else {
        time_format = "%I:%M";
    }
    date_format = "%h %e";
#endif  
    
    strftime(time_text, sizeof(time_text), time_format, tick_time);
    strftime(date_text, sizeof(date_text), date_format, tick_time);
  
    if (time_text[0] == '0') {
        memmove(time_text, &time_text[1], sizeof(time_text) - 1);
    }
    
    if (s_canvas_layer) {
        //layer_mark_dirty(s_canvas_layer);
        if(time_layer)
            text_layer_set_text(time_layer, time_text);
        if(date_layer)
            text_layer_set_text(date_layer, date_text);
        if(batt_layer)
            text_layer_set_text(batt_layer, batt_text);
    }

    s_last_time.hours = tick_time->tm_hour;
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "time hours 1: %d", s_last_time.hours);
    s_last_time.hours -= (s_last_time.hours > 12) ? 12 : 0;
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "time hours 2: %d", s_last_time.hours);
    s_last_time.minutes = tick_time->tm_min;
    
}

static void tap_timer_callback2(void *data) {   
    in_tap_handler = false;
    layer_mark_dirty(s_canvas_layer);
}

static void tap_timer_callback(void *data) {   
    show_bg = !show_bg;
    layer_mark_dirty(s_canvas_layer);
    app_timer_register(tap_sec * 1000, tap_timer_callback2, NULL);
}

static void tap_handler(AccelAxisType axis, int32_t direction) {
#if 0 // debug
    tap_last_axis = axis;
    tap_last_direction = direction;
    if (axis == ACCEL_AXIS_Z) 
    {
        taps++;
        clock_refresh(NULL);
        layer_mark_dirty(s_canvas_layer);  
    }
#endif
    if (!in_tap_handler) {
        in_tap_handler = true;
        show_bg = !show_bg;
        layer_mark_dirty(s_canvas_layer);  
        APP_LOG(APP_LOG_LEVEL_DEBUG, "send_cmd for tap_handler");
        send_cmd();
        app_timer_register(tap_sec * 1000, tap_timer_callback, NULL);
    }
}

static void tick_handler(struct tm * tick_time, TimeUnits changed) {
    if (!(changed & MINUTE_UNIT)) {
        clock_refresh(tick_time);
        return;
    }

    if (!has_launched) {                 
            snprintf(time_delta_str, 12, "load(%d)", check_count + 1);
            
            if (time_delta_layer) {
                text_layer_set_text(time_delta_layer, time_delta_str);
            }
    } else 
    
    if(((t_delta > retry_interval) || (check_count > 1)) &&
       (((!old_bg) && (check_count <= num_checks_throttle)) ||
        ((old_bg || (check_count > num_checks_throttle)) && ((t_delta % retry_interval) == 1)))) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "send_cmd for tick_handler");
        send_cmd();
    } else {   
        if (has_launched) {
            if (t_delta <= 0) {
                t_delta = 0;
                snprintf(time_delta_str, 12, "now"); // puts string into buffer
            } else if (t_delta > 99) {
                snprintf(time_delta_str, 12, "%d m", t_delta); // puts string into buffer
            } else {
                snprintf(time_delta_str, 12, "%d min", t_delta); // puts string into buffer
            }
            if (time_delta_layer) {
                text_layer_set_text(time_delta_layer, time_delta_str);
            }
        }
    }
    t_delta++;  
    clock_refresh(tick_time);

    BatteryChargeState batt_state = battery_state_service_peek();
    int batt_percent = batt_state.charge_percent;
    snprintf(batt_text, sizeof(batt_text), "\n%u%%", batt_percent);
}

static int bgoffset=22;

#ifdef PBL_PLATFORM_CHALK
static int infoCircRadius = 28;
#define infoCirc1 GPoint(33+8, 90)
#define BG_ORIGIN_SIZE GRect(5+9, 68, 54, 48)
#define BG_SYSTEM_FONT_NAME FONT_KEY_GOTHIC_28
#define DELTA_LAYER_ORIGIN_SIZE GRect(5+6, 92, 64, 25)

#define infoCirc2 GPoint(89, 47)
#define infoCirc2Rect GRect(89-28, 47-28, 56, 56)
#define ICON_LAYER_ORIGIN_SIZE GRect(58+17, 32, 30, 30)
#define IOB_LAYER_ORIGIN_SIZE GRect(4+17, 26, 136, 25)
#define COB_LAYER_ORIGIN_SIZE GRect(4+17, 42, 136, 25)

#define infoCirc3 GPoint(89, 133)
#define CHART_ORIGIN { 44+19, 109}
#define CHART_SIZE { 52, 36 }
#define TIME_DELTA_LAYER_ORIGIN_SIZE GRect(0, 135, 180, 25)

#define BATT_LAYER_ORIGIN_SIZE GRect(134, 70, 34, 42)

#else // !PBL_PLATFORM_CHALK
static int infoCircRadius = 28;
#define infoCirc1 GPoint(33-5, 84)
#define BG_ORIGIN_SIZE GRect(5-4, 62, 54, 48)
#define BG_SYSTEM_FONT_NAME FONT_KEY_GOTHIC_28
#define DELTA_LAYER_ORIGIN_SIZE GRect(5-7, 86, 64, 25)

#define infoCirc2 GPoint(71, 39)
#define infoCirc2Rect GRect(71-28, 39-28, 56, 56)
#define ICON_LAYER_ORIGIN_SIZE GRect(58-1, 24, 30, 30)
#define IOB_LAYER_ORIGIN_SIZE GRect(4-1, 18, 136, 25)
#define COB_LAYER_ORIGIN_SIZE GRect(4-1, 35, 136, 25)

#define infoCirc3 GPoint(72-1, 129)
#define CHART_ORIGIN { 44+1, 105}
#define CHART_SIZE { 52, 36 }
#define TIME_DELTA_LAYER_ORIGIN_SIZE GRect(0, 130, 144, 25)

#define BATT_LAYER_ORIGIN_SIZE GRect(105, 63, 34, 42)
#endif

static void update_proc(Layer * layer, GContext * ctx) {
    if(iob_layer)
        text_layer_set_text(iob_layer, iob_str);
    if(cob_layer)
        text_layer_set_text(cob_layer, cob_str);
    if(date_layer)
        text_layer_set_text_color(date_layer, FOREGROUND_COLOR);
    if(batt_layer)
        text_layer_set_text_color(batt_layer, FOREGROUND_COLOR);

    if (delta_layer) {
      if (is_snoozed()) {
          int snooze_left;
          if (alert_snooze == -1)
            snooze_left = 99;
          else
            snooze_left = (alert_snooze - time(NULL)) / 60;
          if (snooze_left < 0)
            snooze_left = 0;
          snprintf(snooze_str, 12, "z:%dm", snooze_left);
          text_layer_set_text(delta_layer, snooze_str);
        }
    }

    graphics_context_set_stroke_color(ctx, BACKGROUND_COLOR); 
    graphics_context_set_antialiased(ctx, ANTIALIASING);

    // time/date background/outline:
    // graphics_context_set_fill_color(ctx, GColorWhite);
    // graphics_fill_rect(ctx, GRect(0, 0, 144, bgoffset+25), 0, GCornerNone);
    
    if (!show_bg) {
        text_layer_set_text(bg_layer, time_text);
    } else {
        text_layer_set_text(bg_layer, bg_text);
    }

    // bg background/outline:
    if ((s_color_channels[0] == 0) &&
        (s_color_channels[1] == 255) &&
        (s_color_channels[2] == 0) &&
        (b_color_channels[0] == 0) &&
        (b_color_channels[1] == 0) &&
        (b_color_channels[2] == 0)
       ) {
        // main background (b_color_channels is either BLACK or RED, in case of old data/comm errors):
        graphics_context_set_fill_color(ctx, BACKGROUND_COLOR);
        graphics_fill_rect(ctx, GRect(0, 0, 180, 180), 0, GCornerNone);

        graphics_context_set_fill_color(ctx, COMP1_BGCOLOR);
        graphics_fill_circle(ctx, infoCirc1, infoCircRadius);
        if(bg_layer)
            text_layer_set_text_color(bg_layer, COMP1_FGCOLOR);
        if(delta_layer)
            text_layer_set_text_color(delta_layer, COMP1_FGCOLOR);

        // set strikethrough color
        graphics_context_set_stroke_color(ctx, COMP1_FGCOLOR);
    } else {
#if defined(PBL_BW)
        if (gcolor_equal(COMP1_BGCOLOR, GColorLightGray) ||
            gcolor_equal(COMP1_BGCOLOR, GColorDarkGray)) {
          s_color_channels[0] = 255;
          s_color_channels[1] = 255;
          s_color_channels[2] = 255;
        } else {
          s_color_channels[0] = 170;
          s_color_channels[1] = 170;
          s_color_channels[2] = 170;
        }
#endif
        // main background (b_color_channels is either BLACK or RED, in case of old data/comm errors):
        graphics_context_set_fill_color(ctx, BACKGROUND_COLOR);
        // but, we ignore it anyway & just use custom bg color, only the bg circle will change color for alerts.
        // graphics_context_set_fill_color(ctx, GColorFromRGB(b_color_channels[0], b_color_channels[1], b_color_channels[2]));
        graphics_fill_rect(ctx, GRect(0, 0, 180, 180), 0, GCornerNone);

        if (gcolor_equal(COMP1_BGCOLOR, GColorFromRGB(s_color_channels[0], s_color_channels[1], s_color_channels[2]))) {
          if (gcolor_equal(GColorRed, GColorFromRGB(s_color_channels[0], s_color_channels[1], s_color_channels[2]))) {
            graphics_context_set_fill_color(ctx, GColorYellow);
          } else {
            graphics_context_set_fill_color(ctx, GColorRed);
          }
        } else {
          graphics_context_set_fill_color(ctx, GColorFromRGB(s_color_channels[0], s_color_channels[1], s_color_channels[2]));
        }
        graphics_fill_circle(ctx, infoCirc1, infoCircRadius);
        if(bg_layer)
            text_layer_set_text_color(bg_layer, GColorBlack);
        if(delta_layer)
            text_layer_set_text_color(delta_layer, GColorBlack);

        // set strikethrough color
        graphics_context_set_stroke_color(ctx, GColorBlack);
    }
  
    // draw strikethrough:
    if (old_bg && show_bg) {
      GPoint left, right;
      left.x = BG_ORIGIN_SIZE.origin.x + 5;
      left.y = BG_ORIGIN_SIZE.origin.y + BG_ORIGIN_SIZE.size.h / 2 - 4;
      right = left;
      right.x += BG_ORIGIN_SIZE.size.w - 10;
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_line(ctx, left, right);
    }
  
    graphics_context_set_fill_color(ctx, COMP2_BGCOLOR);
    graphics_fill_circle(ctx, infoCirc2, infoCircRadius);
    if(time_delta_layer)
        text_layer_set_text_color(time_delta_layer, COMP3_FGCOLOR);

    graphics_context_set_fill_color(ctx, COMP3_BGCOLOR);
    graphics_fill_circle(ctx, infoCirc3, infoCircRadius);
    
    int32_t angleStart=270, angleEnd=270;
    int32_t angleSize = 30;
    int32_t trendInset = 5;
    switch(trend_dir) {
        case 1: //    RESOURCE_ID_IMAGE_UPUP_WHITE,     //0 - 1
            angleStart=0-(2*angleSize); angleEnd=0+(2*angleSize); trendInset = 6; break;
        case 2: //    RESOURCE_ID_IMAGE_UP_WHITE,       //1 - 2
            angleStart=0-angleSize; angleEnd=0+angleSize; break;
        case 3: //    RESOURCE_ID_IMAGE_UP45_WHITE,     //2 - 3
            angleStart=45-angleSize; angleEnd=45+angleSize; break;
        case 4: //    RESOURCE_ID_IMAGE_FLAT_WHITE,     //3 - 4
            angleStart=90-angleSize; angleEnd=90+angleSize; break;
        case 5: //    RESOURCE_ID_IMAGE_DOWN45_WHITE,   //5 - 5
            angleStart=135-angleSize; angleEnd=135+angleSize; break;
        case 6: //    RESOURCE_ID_IMAGE_DOWN_WHITE,     //6 - 6
            angleStart=180-angleSize; angleEnd=180+angleSize; break;
        case 7: //    RESOURCE_ID_IMAGE_DOWNDOWN_WHITE, //7 - 7
            angleStart=180-(2*angleSize); angleEnd=180+(2*angleSize); trendInset = 6; break;
    }
    graphics_context_set_fill_color(ctx, COMP2_FGCOLOR);
    graphics_fill_radial(ctx, infoCirc2Rect, GOvalScaleModeFitCircle, trendInset, DEG_TO_TRIGANGLE(angleStart), DEG_TO_TRIGANGLE(angleEnd));

    if (!in_tap_handler) {
      int32_t trendAngle = TRIG_MAX_ANGLE * (angleStart + angleEnd) / 2 / 360;
      int32_t trend_hand_length = infoCircRadius - trendInset;
      GPoint trend_hand = {
        .x = (int16_t)(sin_lookup(trendAngle) * (int32_t)trend_hand_length / TRIG_MAX_RATIO) + infoCirc2.x,
        .y = (int16_t)(-cos_lookup(trendAngle) * (int32_t)trend_hand_length / TRIG_MAX_RATIO) + infoCirc2.y,
      };
      graphics_context_set_stroke_color(ctx, COMP2_FGCOLOR);
      graphics_context_set_stroke_width(ctx, 1);
      graphics_draw_line(ctx, trend_hand, infoCirc2);
    }

    // chart color:
    if(chart_layer)
        chart_layer_set_plot_color(chart_layer, COMP3_FGCOLOR);
    text_layer_set_text_color(iob_layer, COMP2_FGCOLOR);
    text_layer_set_text_color(cob_layer, COMP2_FGCOLOR);

    graphics_context_set_stroke_color(ctx, FOREGROUND_COLOR); 
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_circle(ctx, infoCirc1, infoCircRadius);
    graphics_draw_circle(ctx, infoCirc2, infoCircRadius);
    graphics_draw_circle(ctx, infoCirc3, infoCircRadius);
}

static void reset_background() {
    
    b_color_channels[0] = 0;
    b_color_channels[1] = 0;
    b_color_channels[2] = 0;
    update_hand_color();

    //if(time_layer)
    //    text_layer_set_text_color(time_layer, GColorBlack);
    layer_mark_dirty(s_canvas_layer);
}

static void process_alert() {
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "Vibe State: %i", vibe_state);
    switch (alert_state) {
        
    case LOSS_MID_NO_NOISE:;    
        s_color_channels[0] = 255;
        s_color_channels[1] = 255;
        s_color_channels[2] = 0;
        
        if (vibe_state > 0 && !is_snoozed())
            vibes_long_pulse();
            
        //APP_LOG(APP_LOG_LEVEL_DEBUG, "Alert key: %i", LOSS_MID_NO_NOISE);
#if defined(PBL_COLOR)
#ifdef PBL_PLATFORM_CHALK 
        if(bg_layer)
            text_layer_set_text_color(bg_layer, GColorBlack);
        if(delta_layer)       
            text_layer_set_text_color(delta_layer, GColorBlack);
#else 
        if(bg_layer)
            text_layer_set_text_color(bg_layer, GColorBlack);
        if(delta_layer)
            text_layer_set_text_color(delta_layer, GColorBlack);
#endif
        //bitmap_layer_set_compositing_mode(icon_layer, GCompOpClear);
#elif defined(PBL_BW)
        s_color_channels[0] = 170;
        s_color_channels[1] = 170;
        s_color_channels[2] = 170;
        if(bg_layer)
            text_layer_set_text_color(bg_layer, GColorBlack);
        if(delta_layer)
            text_layer_set_text_color(delta_layer, GColorBlack);
        //if(icon_layer)
        //    bitmap_layer_set_compositing_mode(icon_layer, GCompOpClear);
#endif
                     
        break;
        
    case LOSS_HIGH_NO_NOISE:;
        s_color_channels[0] = 255;
        s_color_channels[1] = 0;
        s_color_channels[2] = 0;
        
       if (vibe_state > 0 && !is_snoozed())
            vibes_long_pulse();
            
#ifdef PBL_PLATFORM_CHALK
        if(delta_layer)
            text_layer_set_text_color(delta_layer, GColorBlack);
#else   
        if(delta_layer)   
            text_layer_set_text_color(delta_layer, GColorWhite);
#endif

        if(bg_layer)
            text_layer_set_text_color(bg_layer, GColorWhite);
        //if(icon_layer)
            //bitmap_layer_set_compositing_mode(icon_layer, GCompOpOr);
        break;
        
    case OKAY:;

        s_color_channels[0] = 0;
        s_color_channels[1] = 255;
        s_color_channels[2] = 0;
        
        if (vibe_state > 1 && !is_snoozed())
            vibes_double_pulse();
            
        //APP_LOG(APP_LOG_LEVEL_DEBUG, "Alert key: %i", OKAY);
        if(bg_layer)
            text_layer_set_text_color(bg_layer, FOREGROUND_COLOR);
        if(delta_layer)   
            text_layer_set_text_color(delta_layer, FOREGROUND_COLOR);
        //if(icon_layer)
        //    bitmap_layer_set_compositing_mode(icon_layer, GCompOpClear);
        break;
        
    case OLD_DATA:;

        comm_alert();
        //APP_LOG(APP_LOG_LEVEL_DEBUG, "Alert key: %i", OLD_DATA);
        
        s_color_channels[0] = 0;
        s_color_channels[1] = 0;
        s_color_channels[2] = 255;
        
#ifdef PBL_PLATFORM_CHALK
        if(delta_layer)
            text_layer_set_text_color(delta_layer, GColorBlack);
#else   
        if(delta_layer)   
            text_layer_set_text_color(delta_layer, GColorWhite);
#endif

        //if (icon_layer)
        //    bitmap_layer_set_compositing_mode(icon_layer, GCompOpOr);
        if (bg_layer)
            text_layer_set_text_color(bg_layer, GColorWhite);

        break;
     
     case NO_CHANGE:;
        break;   
    }

    update_hand_color();
}

static GColor colorLookup(char *cstr, GColor gcolor) {
    // list from https://developer.pebble.com/docs/c/Graphics/Graphics_Types/Color_Definitions/
    typedef struct colorList_t {
        GColor gCol;
        char *cStr;
    } ColorListT;
    ColorListT cList[] = {
        {GColorWhite, "white"},
        {GColorBlack, "black"},
        {GColorOxfordBlue, "oxfordblue"},
        {GColorDukeBlue, "dukeblue"},
        {GColorBlue, "blue"},
        {GColorDarkGreen, "darkgreen"},
        {GColorMidnightGreen, "midnightgreen"},
        {GColorCobaltBlue, "cobaltblue"},
        {GColorBlueMoon, "bluemoon"},
        {GColorIslamicGreen, "islamicgreen"},
        {GColorJaegerGreen, "jaegergreen"},
        {GColorTiffanyBlue, "tiffanyblue"},
        {GColorVividCerulean, "vividcerulean"},
        {GColorGreen, "green"},
        {GColorMalachite, "malachite"},
        {GColorMediumSpringGreen, "mediumspringgreen"},
        {GColorCyan, "cyan"},
        {GColorBulgarianRose, "bulgarianrose"},
        {GColorImperialPurple, "imperialpurple"},
        {GColorIndigo, "indigo"},
        {GColorElectricUltramarine, "electricultramarine"},
        {GColorArmyGreen, "armygreen"},
        {GColorDarkGray, "darkgray"},
        {GColorLiberty, "liberty"},
        {GColorVeryLightBlue, "verylightblue"},
        {GColorKellyGreen, "kellygreen"},
        {GColorMayGreen, "maygreen"},
        {GColorCadetBlue, "cadetblue"},
        {GColorPictonBlue, "pictonblue"},
        {GColorBrightGreen, "brightgreen"},
        {GColorScreaminGreen, "screamingreen"},
        {GColorMediumAquamarine, "mediumaquamarine"},
        {GColorElectricBlue, "electricblue"},
        {GColorDarkCandyAppleRed, "darkcandyapplered"},
        {GColorJazzberryJam, "jazzberryjam"},
        {GColorPurple, "purple"},
        {GColorVividViolet, "vividviolet"},
        {GColorWindsorTan, "windsortan"},
        {GColorRoseVale, "rosevale"},
        {GColorPurpureus, "purpureus"},
        {GColorLavenderIndigo, "lavenderindigo"},
        {GColorLimerick, "limerick"},
        {GColorBrass, "brass"},
        {GColorLightGray, "lightgray"},
        {GColorBabyBlueEyes, "babyblueeyes"},
        {GColorSpringBud, "springbud"},
        {GColorInchworm, "inchworm"},
        {GColorMintGreen, "mintgreen"},
        {GColorCeleste, "celeste"},
        {GColorRed, "red"},
        {GColorFolly, "folly"},
        {GColorFashionMagenta, "fashionmagenta"},
        {GColorMagenta, "magenta"},
        {GColorOrange, "orange"},
        {GColorSunsetOrange, "sunsetorange"},
        {GColorBrilliantRose, "brilliantrose"},
        {GColorShockingPink, "shockingpink"},
        {GColorChromeYellow, "chromeyellow"},
        {GColorRajah, "rajah"},
        {GColorMelon, "melon"},
        {GColorRichBrilliantLavender, "richbrilliantlavender"},
        {GColorYellow, "yellow"},
        {GColorIcterine, "icterine"},
        {GColorPastelYellow, "pastelyellow"},
    };
    for (unsigned i=0; i<sizeof(cList)/sizeof(ColorListT); i++) {
        if (strncmp(cstr, cList[i].cStr, 124) == 0) {
            gcolor = cList[i].gCol;
        }
    }
    return gcolor;
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    check_count = 0;
    // APP_LOG(APP_LOG_LEVEL_INFO, "Message received!");
    if(time_delta_layer)
        text_layer_set_text(time_delta_layer, "in...");
        
    // Get the first pair
    Tuple *new_tuple = dict_read_first(iterator);
    
    uint32_t dict = dict_size(iterator);
    // APP_LOG(APP_LOG_LEVEL_INFO, "size of received: %d", (int)dict);
    reset_background();
    CgmData* cgm_data = cgm_data_create(1,2,"3m","199","+3mg/dL","Evan");

    // Process all pairs present
    while(new_tuple != NULL) {
        // Process this pair's key      
        switch (new_tuple->key) {
                
            case CGM_ID:;
                data_id = new_tuple->value->int32;
                break;
                
            case CGM_EGV_DELTA_KEY:;
                if(delta_layer)
                    text_layer_set_text(delta_layer, new_tuple->value->cstring);
                old_bg = false;
                if (strncmp(new_tuple->value->cstring, "old", 4) == 0) {
                  old_bg = true;
                }
                break;
                
            case CGM_EGV_KEY:;
                cgm_data_set_egv(cgm_data, new_tuple->value->cstring);

                if(bg_layer)
                    strcpy(bg_text, cgm_data_get_egv(cgm_data));

                strncpy(last_bg, new_tuple->value->cstring, 124);
                break;
                
            case CGM_TREND_KEY:;
                //if (icon_bitmap) {
                //    gbitmap_destroy(icon_bitmap);
                //}
                //icon_bitmap = gbitmap_create_with_resource(CGM_ICONS[new_tuple->value->uint8]);
                trend_dir = new_tuple->value->uint8;
                //if(icon_layer)
                //    bitmap_layer_set_bitmap(icon_layer, icon_bitmap);
                break;
                
            case CGM_ALERT_KEY:;
                alert_state = new_tuple->value->uint8;
                break;
                
            case CGM_VIBE_KEY:
                vibe_state = new_tuple->value->int16;             
                break;
                
            case CGM_IOB_KEY:;
                strncpy(iob_str, new_tuple->value->cstring, 124);
                break;
            
            case CGM_COB_KEY:;
                strncpy(cob_str, new_tuple->value->cstring, 124);
                // APP_LOG(APP_LOG_LEVEL_DEBUG, "COB: %s", new_tuple->value->cstring);
                break;
            
            case CGM_TIME_DELTA_KEY:;
                int t_delta_temp = new_tuple->value->int16;
                
                if (t_delta_temp < 0) {
                    t_delta = t_delta;
                }else {
                    t_delta = t_delta_temp;
                }
                
                if (t_delta <= 0) {
                    t_delta = 0;
                    snprintf(time_delta_str, 12, "now"); // puts string into buffer
                } else if (t_delta > 99) {
                    snprintf(time_delta_str, 12, "%d m", t_delta); // puts string into buffer
                } else {
                    snprintf(time_delta_str, 12, "%d min", t_delta); // puts string into buffer
                }
                if (time_delta_layer) {
                    text_layer_set_text(time_delta_layer, time_delta_str);
                }
                break;
            case CGM_BGS:;
                ProcessingState* state = data_processor_create(new_tuple->value->cstring, ',');
                uint8_t num_strings = data_processor_count(state);
                //APP_LOG(APP_LOG_LEVEL_DEBUG, "BG num: %i", num_strings);
                bgs = (int*)malloc((num_strings-1)*sizeof(int));     
                for (uint8_t n = 0; n < num_strings; n += 1) {
                    if (n == 0) {
                        is_web = data_processor_get_int(state);
                        //APP_LOG(APP_LOG_LEVEL_DEBUG, "Tag Raw : %i", tag_raw);
                    }
                    else {
                        bgs[n-1] = data_processor_get_int(state);
                        //APP_LOG(APP_LOG_LEVEL_DEBUG, "BG Split: %i", bgs[n-1]);
                    }     
                }
                num_bgs = num_strings - 1;			
                break;
            case CGM_BG_TIMES:;
                ProcessingState* state_t = data_processor_create(new_tuple->value->cstring, ',');
                uint8_t num_strings_t = data_processor_count(state_t);
                //APP_LOG(APP_LOG_LEVEL_DEBUG, "BG_t num: %i", num_strings_t);
                bg_times = (int*)malloc(num_strings_t*sizeof(int));
                for (uint8_t n = 0; n < num_strings_t; n += 1) {
                    bg_times[n] = data_processor_get_int(state_t);
                    //APP_LOG(APP_LOG_LEVEL_DEBUG, "BG_t Split: %i", bg_times[n]);
                }			
                break;
            case CGM_HIDE_KEY:
                //APP_LOG(APP_LOG_LEVEL_DEBUG, "HIDE: %d", (int)new_tuple->value->int16);
                if (in_tap_handler) {
                    show_bg = new_tuple->value->int16;
                } else {
                    show_bg = !new_tuple->value->int16;
                }
                break;
            case CGM_SHOWSEC_KEY:
                showsec = new_tuple->value->int16;             
                tick_timer_service_unsubscribe();
                if (showsec)
                    tick_timer_service_subscribe(SECOND_UNIT|MINUTE_UNIT, tick_handler);
                else
                    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
                break;
            case CGM_TAPTIME_KEY:
                tap_sec = new_tuple->value->int32;             
                break;
            case CGM_FGCOLOR_KEY:
                fgcolor = colorLookup(new_tuple->value->cstring, GColorWhite);
                // APP_LOG(APP_LOG_LEVEL_DEBUG, "FGCOLOR: %s", new_tuple->value->cstring);
                break;
            case CGM_BGCOLOR_KEY:
                bgcolor = colorLookup(new_tuple->value->cstring, GColorBlack);
                // APP_LOG(APP_LOG_LEVEL_DEBUG, "BGCOLOR: %s", new_tuple->value->cstring);
                break;
            case CGM_CFGCOLOR_KEY:
                cfgcolor = colorLookup(new_tuple->value->cstring, GColorWhite);
                // APP_LOG(APP_LOG_LEVEL_DEBUG, "CFGCOLOR: %s", new_tuple->value->cstring);
                break;
            case CGM_CBGCOLOR_KEY:
                cbgcolor = colorLookup(new_tuple->value->cstring, GColorBlack);
                // APP_LOG(APP_LOG_LEVEL_DEBUG, "CBGCOLOR: %s", new_tuple->value->cstring);
                break;
        }
        
        // Get next pair, if any
        new_tuple = dict_read_next(iterator);
    }
    
    // Redraw
    if (s_canvas_layer) {
        analog_mode(curBG, curFG, 0);

        //load_chart_1();
        time_t t = time(NULL);
        struct tm * time_now = localtime(&t);
        clock_refresh(time_now);
        layer_mark_dirty(s_canvas_layer);
        if (chart_layer) {
            chart_layer_set_canvas_color(chart_layer, GColorClear);
            chart_layer_set_margin(chart_layer, 7);
#if 0 // debug only:
            int fake_bg_times[] = { 0, 10, 20, 30, 40, 50, 60, 70 };
            int fake_bgs[] = { /* 140, 70, 140, 70, 140, 70, 140, 70,
                               70, 140, 70, 140, 70, 140, 70, 140,
                               */ 130, 133, 138, 131, 133, 128, 130, 126, 123
                               };
            chart_layer_set_data(chart_layer, fake_bg_times, eINT, fake_bgs, eINT, 8);
#else
            chart_layer_set_data(chart_layer, bg_times, eINT, bgs, eINT, num_bgs);
#endif
        }
    }
    //Process Alerts
    process_alert();
    has_launched = 1;
    
    //timer2 = app_timer_register(60000*2, timer_callback_2, NULL);
    

}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_INFO, "inbox_dropped_callback");
    s_color_channels[0] = 0;
    s_color_channels[1] = 0;
    s_color_channels[2] = 255;
    update_hand_color();
        
    //if (time_delta_layer)
    //    text_layer_set_text_color(time_delta_layer, GColorBlack);
    //if (icon_layer)
    //    bitmap_layer_set_compositing_mode(icon_layer, GCompOpClear);
    if (bg_layer)
        text_layer_set_text_color(bg_layer, GColorBlack);
    if (delta_layer)    
        text_layer_set_text_color(delta_layer, GColorBlack);
    
    snprintf(time_delta_str, 12, "in-err(%d)", t_delta);
    if(bg_layer) {
      old_bg = false;
      strcpy(bg_text, translate_error(reason));
    }
        
    if(time_delta_layer)
        text_layer_set_text(time_delta_layer, time_delta_str);
        
    comm_alert();

}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
    s_color_channels[0] = 0;
    s_color_channels[1] = 0;
    s_color_channels[2] = 255;
    update_hand_color();
        
    //if (time_delta_layer)
    //    text_layer_set_text_color(time_delta_layer, GColorBlack);
    //if (icon_layer)
    //    bitmap_layer_set_compositing_mode(icon_layer, GCompOpClear);
    if (bg_layer)
        text_layer_set_text_color(bg_layer, GColorBlack);
    if (delta_layer)    
        text_layer_set_text_color(delta_layer, GColorBlack);
        
    snprintf(time_delta_str, 12, "out-err(%d)", t_delta);
    
    if(bg_layer) {
      old_bg = false;
      strcpy(bg_text, translate_error(reason));
    }
    
    if(time_delta_layer)
        text_layer_set_text(time_delta_layer, time_delta_str);
    comm_alert();

}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
    //APP_LOG(APP_LOG_LEVEL_INFO, "out sent callback");
}

static void window_load(Window * window) {
    Layer * window_layer = window_get_root_layer(window);
    GRect window_bounds = layer_get_bounds(window_layer);
    
    int offset = 24/2;
    GRect inner_bounds = GRect(0, 24, 144, 144);
    s_center = grect_center_point(&inner_bounds);

    s_canvas_layer = layer_create(window_bounds);
    layer_set_update_proc(s_canvas_layer, update_proc);
    layer_add_child(window_layer, s_canvas_layer);

    analog_init(s_canvas_layer);
    analog_mode(curBG, curFG, 0);

    int batteryoffset = 30;
    icon_layer = bitmap_layer_create(ICON_LAYER_ORIGIN_SIZE);
    delta_layer = text_layer_create(DELTA_LAYER_ORIGIN_SIZE);
    time_delta_layer = text_layer_create(TIME_DELTA_LAYER_ORIGIN_SIZE);
    iob_layer = text_layer_create(IOB_LAYER_ORIGIN_SIZE);
    cob_layer = text_layer_create(COB_LAYER_ORIGIN_SIZE);
    time_layer = text_layer_create(GRect(0, 1, 90, bgoffset+25));
    date_layer = text_layer_create(GRect(90, 6, 54, bgoffset+25));  
    batt_layer = text_layer_create(BATT_LAYER_ORIGIN_SIZE);  
    chart_layer = chart_layer_create((GRect) { 
      	.origin = CHART_ORIGIN,
  	  	.size = CHART_SIZE });
    bg_layer = text_layer_create(BG_ORIGIN_SIZE);
    text_layer_set_text_alignment(time_delta_layer, GTextAlignmentCenter); 
    text_layer_set_text_alignment(iob_layer, GTextAlignmentCenter); 
    text_layer_set_text_alignment(cob_layer, GTextAlignmentCenter); 
    text_layer_set_text_alignment(delta_layer, GTextAlignmentCenter);  

    bitmap_layer_set_background_color(icon_layer, GColorClear);
    bitmap_layer_set_compositing_mode(icon_layer, GCompOpSet);
    //layer_add_child(s_canvas_layer, bitmap_layer_get_layer(icon_layer));

    chart_layer_set_plot_color(chart_layer, GColorWhite);
  	chart_layer_set_canvas_color(chart_layer, GColorClear);
  	chart_layer_show_points_on_line(chart_layer, true);
	chart_layer_animate(chart_layer, false);
    // chart_layer_set_plot_type(chart_layer, eLINE)
  	layer_add_child(s_canvas_layer /*window_layer*/, chart_layer_get_layer(chart_layer));

    text_layer_set_text_color(bg_layer, GColorWhite);
    text_layer_set_background_color(bg_layer, GColorClear);
    // text_layer_set_font(bg_layer, fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_HN_BOLD_48)));
    text_layer_set_font(bg_layer, fonts_get_system_font(BG_SYSTEM_FONT_NAME)); 
    text_layer_set_text_alignment(bg_layer, GTextAlignmentCenter);
    layer_add_child(s_canvas_layer, text_layer_get_layer(bg_layer)); 
 
    text_layer_set_text_color(delta_layer, GColorWhite);
    text_layer_set_background_color(delta_layer, GColorClear);
    text_layer_set_font(delta_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(s_canvas_layer, text_layer_get_layer(delta_layer)); 
 
    text_layer_set_text_color(time_delta_layer, GColorWhite);
    text_layer_set_background_color(time_delta_layer, GColorClear);
    text_layer_set_font(time_delta_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(s_canvas_layer, text_layer_get_layer(time_delta_layer)); 
 
    text_layer_set_text_color(iob_layer, GColorWhite);
    text_layer_set_background_color(iob_layer, GColorClear);
    text_layer_set_font(iob_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(s_canvas_layer, text_layer_get_layer(iob_layer)); 

    text_layer_set_text_color(cob_layer, GColorWhite);
    text_layer_set_background_color(cob_layer, GColorClear);
    text_layer_set_font(cob_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(s_canvas_layer, text_layer_get_layer(cob_layer)); 

    text_layer_set_text_color(time_layer, GColorWhite);
    text_layer_set_background_color(time_layer, GColorClear);
    text_layer_set_text_color(date_layer, GColorWhite);
    text_layer_set_background_color(date_layer, GColorClear);
    text_layer_set_text_color(batt_layer, GColorWhite);
    text_layer_set_background_color(batt_layer, GColorClear);
    // 
    
    #ifdef PBL_PLATFORM_CHALK   
    text_layer_set_font(time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD)); 
    text_layer_set_font(date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD)); 
    text_layer_set_font(batt_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18)); 
    #else 
    text_layer_set_font(time_layer, fonts_get_system_font(FONT_KEY_BITHAM_34_MEDIUM_NUMBERS)); 
    text_layer_set_font(date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD)); 
    text_layer_set_font(batt_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18)); 
    #endif
 
    text_layer_set_text_alignment(time_layer, GTextAlignmentCenter);
    // layer_add_child(s_canvas_layer, text_layer_get_layer(time_layer));
    text_layer_set_text_alignment(date_layer, GTextAlignmentCenter);
    // layer_add_child(s_canvas_layer, text_layer_get_layer(date_layer));
    text_layer_set_text_alignment(batt_layer, GTextAlignmentCenter);
    layer_add_child(s_canvas_layer, text_layer_get_layer(batt_layer));
    
    strcpy(bg_text, "cgm");

    analog_window_load(s_canvas_layer);
}

static void window_unload(Window * window) {
    analog_deinit();
    analog_window_unload();
    layer_destroy(s_canvas_layer);
}

/*********************************** App **************************************/
static void init() {
    curFG = fgcolor = cfgcolor = GColorWhite;
    curBG = bgcolor = cbgcolor = GColorBlack;
    
    time_t t = time(NULL);
    show_bg = true;

    if(launch_reason() == APP_LAUNCH_TIMELINE_ACTION) {
        uint32_t arg = launch_get_args();
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Launch: %i", (int)arg);
        if (arg == 2) {
          alert_snooze = 0;    
          persist_write_int(SNOOZE_KEY, alert_snooze);
          APP_LOG(APP_LOG_LEVEL_DEBUG, "cancel mute");
        } else if (arg == 3) {
          alert_snooze = -1;  
          persist_write_int(SNOOZE_KEY, alert_snooze);
          APP_LOG(APP_LOG_LEVEL_DEBUG, "mute until cancelled");
        } else if (arg > 3) {
          alert_snooze = t + arg*60;  
          persist_write_int(SNOOZE_KEY, alert_snooze);
          APP_LOG(APP_LOG_LEVEL_DEBUG, "mute for: %i", (int)alert_snooze);
        }
    }

    if (persist_exists(SNOOZE_KEY)) {
        alert_snooze = persist_read_int(SNOOZE_KEY);
    }
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Snooze Exp: %i", (int)alert_snooze);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "time: %i", (int)t);
    struct tm * time_now = localtime(&t);
    tick_handler(time_now, MINUTE_UNIT);
    
    s_main_window = window_create();
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = window_load,
        .unload = window_unload,
    });
    window_stack_push(s_main_window, true);
    
    if (showsec)
        tick_timer_service_subscribe(SECOND_UNIT|MINUTE_UNIT, tick_handler);
    else
        tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
    
    // accel_service_set_sampling_rate(ACCEL_SAMPLING_10HZ);
    accel_tap_service_subscribe(tap_handler);
    
       
    // Registering callbacks
    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_register_outbox_sent(outbox_sent_callback);
    int maxMessageBytes = 1 + 17/*# of tuples*/ * 7 +
                   16/*delta*/ + 8/*egv*/ + 4/*trend*/ + 4/*alert*/ + 4/*vibe*/ + 8/*wall*/ + 4/*deltaminutes*/ + 33/*bgarray*/ + 41/*bgtimearray*/ +
                   8/*iob*/ + 8/*cob*/ + 4/*showsec*/ + 4/*hide*/ + 4/*taptime*/ + 20/*fgcolor*/ + 20/*bgcolor*/ + 20/*cfgcolor*/ + 20/*cbgcolor*/;
    APP_LOG(APP_LOG_LEVEL_DEBUG, "maxMessageBytes: %i, max: %i", maxMessageBytes, (int)app_message_inbox_size_maximum());
    AppMessageResult ropen = app_message_open(2 * maxMessageBytes, 40);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "app_message_open: %i", (int)ropen);
    
    timer = app_timer_register(1000, timer_callback, NULL);
}

static void deinit() {
    accel_tap_service_unsubscribe();
    window_destroy(s_main_window);
}

int main() {
    init();
    app_event_loop();
    deinit();
}
