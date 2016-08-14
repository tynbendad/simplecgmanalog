#include <pebble.h>
#include "analog.h"

#define BACKGROUND_COLOR bg_color
#define FOREGROUND_COLOR fg_color
#define ANTIALIASING true

extern bool in_tap_handler;
extern bool showsec;

static Layer *s_simple_bg_layer, *s_date_layer, *s_hands_layer;
static TextLayer *s_day_label, *s_num_label;

static GPath *s_tick_paths[NUM_CLOCK_TICKS];
static GPath *s_minute_arrow, *s_hour_arrow;
static char s_num_buffer[4], s_day_buffer[6];
static GColor bg_color;
static GColor fg_color;

static void analog_hands_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GPoint center = grect_center_point(&bounds);
    
  {
      graphics_context_set_fill_color(ctx, FOREGROUND_COLOR);
      for (int i = 0; i < NUM_CLOCK_TICKS; ++i) {
        const int x_offset = PBL_IF_ROUND_ELSE(18, 0);
        const int y_offset = PBL_IF_ROUND_ELSE(6, 0);
        gpath_move_to(s_tick_paths[i], GPoint(x_offset, y_offset));
        gpath_draw_filled(ctx, s_tick_paths[i]);
      }

      time_t now = time(NULL);
      struct tm *t = localtime(&now);

      if (showsec) {
          const int16_t second_hand_length = PBL_IF_ROUND_ELSE((bounds.size.h / 2) - 6, bounds.size.h / 2);

          int32_t second_angle = TRIG_MAX_ANGLE * t->tm_sec / 60;
          GPoint second_hand = {
              .x = (int16_t)(sin_lookup(second_angle) * (int32_t)second_hand_length / TRIG_MAX_RATIO) + center.x,
              .y = (int16_t)(-cos_lookup(second_angle) * (int32_t)second_hand_length / TRIG_MAX_RATIO) + center.y,
          };

          // second hand
          graphics_context_set_stroke_color(ctx, FOREGROUND_COLOR);
          graphics_draw_line(ctx, second_hand, center);
      }

      // minute/hour hand
      if (in_tap_handler) {
          graphics_context_set_stroke_color(ctx, FOREGROUND_COLOR);
      } else {
          graphics_context_set_fill_color(ctx, FOREGROUND_COLOR);
          graphics_context_set_stroke_color(ctx, BACKGROUND_COLOR);
      }
      
      graphics_context_set_stroke_width(ctx, 1);
      gpath_rotate_to(s_minute_arrow, TRIG_MAX_ANGLE * t->tm_min / 60);
      if (!in_tap_handler)
          gpath_draw_filled(ctx, s_minute_arrow);
      gpath_draw_outline(ctx, s_minute_arrow);
    
      gpath_rotate_to(s_hour_arrow, (TRIG_MAX_ANGLE * (((t->tm_hour % 12) * 6) + (t->tm_min / 10))) / (12 * 6));
      if (!in_tap_handler)
          gpath_draw_filled(ctx, s_hour_arrow);
      gpath_draw_outline(ctx, s_hour_arrow);
    
      // dot in the middle
      graphics_context_set_fill_color(ctx, BACKGROUND_COLOR);
      graphics_fill_rect(ctx, GRect(bounds.size.w / 2 - 1, bounds.size.h / 2 - 1, 3, 3), 0, GCornerNone);
  }
}

static void analog_date_update_proc(Layer *layer, GContext *ctx) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  text_layer_set_text_color(s_day_label, FOREGROUND_COLOR);
  strftime(s_day_buffer, sizeof(s_day_buffer), "%a", t);
  text_layer_set_text(s_day_label, s_day_buffer);

  text_layer_set_text_color(s_num_label, FOREGROUND_COLOR);
  strftime(s_num_buffer, sizeof(s_num_buffer), "%d", t);
  text_layer_set_text(s_num_label, s_num_buffer);
}

//static void analog_handle_second_tick(struct tm *tick_time, TimeUnits units_changed) {
  //layer_mark_dirty(window_get_root_layer(s_window));
//}

void analog_window_load(Layer *window_layer) {
  GRect bounds = layer_get_bounds(window_layer);

  s_date_layer = layer_create(bounds);
  layer_set_update_proc(s_date_layer, analog_date_update_proc);
  layer_add_child(window_layer, s_date_layer);

  // center of PEBBLE TIME display: 72, 84 (size: 144x168); center of ROUND display: 90, 90 (size: 180x180)
  s_day_label = text_layer_create(PBL_IF_ROUND_ELSE(
    GRect(63+63, 114-37-4, 27, 20),
    GRect(46+52, 114-42-7, 27, 20)));
  text_layer_set_text(s_day_label, s_day_buffer);
  text_layer_set_background_color(s_day_label, GColorClear);
  text_layer_set_text_color(s_day_label, FOREGROUND_COLOR);
  text_layer_set_font(s_day_label, fonts_get_system_font(FONT_KEY_GOTHIC_18));

  layer_add_child(s_date_layer, text_layer_get_layer(s_day_label));

  s_num_label = text_layer_create(PBL_IF_ROUND_ELSE(
    GRect(90+60, 114-37-9, 28, 28),
    GRect(73+48, 114-42-12, 28, 28)));
  text_layer_set_text(s_num_label, s_num_buffer);
  text_layer_set_background_color(s_num_label, GColorClear);
  text_layer_set_text_color(s_num_label, FOREGROUND_COLOR);
  text_layer_set_font(s_num_label, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));

  layer_add_child(s_date_layer, text_layer_get_layer(s_num_label));

  s_hands_layer = layer_create(bounds);
  layer_set_update_proc(s_hands_layer, analog_hands_update_proc);
  layer_add_child(window_layer, s_hands_layer);
}

void analog_window_unload() {
  layer_destroy(s_simple_bg_layer);
  layer_destroy(s_date_layer);

  text_layer_destroy(s_day_label);
  text_layer_destroy(s_num_label);

  layer_destroy(s_hands_layer);
}

void analog_mode(GColor bg, GColor fg, int unused) {
  bg_color = bg;
  fg_color = fg;
}

void analog_init(Layer *window_layer) {
  s_day_buffer[0] = '\0';
  s_num_buffer[0] = '\0';

  // init hand paths
  s_minute_arrow = gpath_create(&MINUTE_HAND_POINTS);
  s_hour_arrow = gpath_create(&HOUR_HAND_POINTS);

  // Layer *window_layer = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(window_layer);
  GPoint center = grect_center_point(&bounds);
  gpath_move_to(s_minute_arrow, center);
  gpath_move_to(s_hour_arrow, center);

  for (int i = 0; i < NUM_CLOCK_TICKS; ++i) {
    s_tick_paths[i] = gpath_create(&ANALOG_BG_POINTS[i]);
  }

  // tick_timer_service_subscribe(SECOND_UNIT, analog_handle_second_tick);
}

void analog_deinit() {
  gpath_destroy(s_minute_arrow);
  gpath_destroy(s_hour_arrow);

  for (int i = 0; i < NUM_CLOCK_TICKS; ++i) {
    gpath_destroy(s_tick_paths[i]);
  }

  // tick_timer_service_unsubscribe();
}
