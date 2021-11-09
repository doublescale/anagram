#define write_chars(cs) write(STDOUT_FILENO, cs, sizeof(cs) - 1)

typedef enum
{
  MODIFIER_SHIFT = 0x01,
  MODIFIER_CTRL  = 0x02,
  MODIFIER_ALT   = 0x04,
} modifiers_t;

typedef struct
{
  u8 half_transitions;
  u8 ended_down;
} game_button_state_t;

typedef struct
{
  game_button_state_t btn_mouse_left;
  game_button_state_t btn_mouse_middle;
  game_button_state_t btn_mouse_right;
  v2i mouse_pos;
  i32 mouse_scroll_y;
  modifiers_t modifiers_held;

  u32 typed_key_count;
  u8* typed_keys;
} live_input_t;

internal b32 ended_down(game_button_state_t state)
{
  return state.ended_down;
}

internal i32 count_down_transitions(game_button_state_t state)
{
  return !state.ended_down
    ? state.half_transitions / 2
    : (state.half_transitions + 1) / 2;
}

internal i32 count_up_transitions(game_button_state_t state)
{
  return state.ended_down
    ? state.half_transitions / 2
    : (state.half_transitions + 1) / 2;
}

internal b32 went_down(game_button_state_t state)
{
  return count_down_transitions(state) > 0;
}

internal b32 went_up(game_button_state_t state)
{
  return count_up_transitions(state) > 0;
}

internal i32 button_toggled(game_button_state_t state)
{
  return (count_down_transitions(state) % 2) == 1;
}

typedef struct
{
  v3u8 fg_col;
  v3u8 bg_col;
  u8 chr;
} color_char_t;

typedef struct
{
  i32 width;
  i32 height;
  i32 pitch;
  color_char_t* chars;
  color_char_t* chars_last;
  b32 full_redraw;
} char_frame_t;

typedef struct
{
  pthread_mutex_t mutex;
  i32 key_count;
  u8 key_buf[1024];
} input_thread_data_t;

typedef struct
{
  struct termios termattr_orig;
  u8 typed_key_buffer[256];
  input_thread_data_t input_thread_data;

  b32 draw_debug_info;
  b32 use_colors;
  b32 use_16colors;

  live_input_t input_persist;
} terminal_context_t;

internal i32 color_24bit_to_16color_code(v3u8 color)
{
  i32 r_bits = (((i32)color.r) & 0x1FF) >> 6;
  i32 g_bits = (((i32)color.g) & 0x1FF) >> 6;
  i32 b_bits = (((i32)color.b) & 0x1FF) >> 6;

  i32 result = 0;

  // TODO: Tweak this.
  if(r_bits == 3 && g_bits == 3 && b_bits == 3)
  {
    // Full white.
    result = 67;
  }
  else if(r_bits == 3 && g_bits < 3 && b_bits < 3)
  {
    // Bright red.
    result = 61;
  }
  else if(r_bits < 3 && g_bits == 3 && b_bits < 3)
  {
    // Bright green.
    result = 62;
  }
  else if(r_bits == 3 && g_bits == 3 && b_bits < 3)
  {
    // Bright yellow.
    result = 63;
  }
  else if(r_bits < 3 && g_bits < 3 && b_bits == 3)
  {
    // Bright blue.
    result = 64;
  }
  else if(r_bits == 3 && g_bits < 3 && b_bits == 3)
  {
    // Bright magenta.
    result = 65;
  }
  else if(r_bits < 3 && g_bits == 3 && b_bits == 3)
  {
    // Bright cyan.
    result = 66;
  }
  else if(r_bits == 2 && g_bits == 2 && b_bits == 2)
  {
    // Bright gray.
    result = 7;
  }
  else if(r_bits == 1 && g_bits == 1 && b_bits == 1)
  {
    // Dark gray.
    result = 60;
  }
  else
  {
    result = (r_bits >> 1) | ((g_bits >> 1) << 1) | ((b_bits >> 1) << 2);
  }

  return result;
}

internal void print_frame(terminal_context_t* ctx, char_frame_t* frame)
{
  b32 use_colors = ctx->use_colors;
  b32 use_16colors = ctx->use_16colors;
  b32 draw_debug_info = ctx->draw_debug_info;

  write_chars("\e[H");  // Move cursor to top left.

  u8 screenbuf[2 * 1024 * 1024];  // TODO: Determine precise upper bound; allocate with frame?
  i32 screenbuf_size = 0;
  color_char_t prev_char = {
    .fg_col = {255, 255, 255},
    .bg_col = {0, 0, 0},
    .chr = ' ',
  };
  v2i last_drawn_coords = {0, 0};

  for(i32 j = frame->height - 1;
      j >= 0;
      --j)
  {
    for(i32 i = 0;
        i < frame->width;
        ++i)
    {
      // Turn non-printable characters to spaces.
      color_char_t col_c = *(color_char_t*)((u8*)(frame->chars + i) + j * frame->pitch);
      if(col_c.chr < ' ' || col_c.chr > '~')
      {
        col_c.chr = ' ';
      }

      // TODO: Be smarter about dirty regions, compare only necessary color data.
      color_char_t* last_char = (color_char_t*)((u8*)(frame->chars_last + i) + j * frame->pitch);
      if(frame->full_redraw ||
          last_char->chr != col_c.chr ||
          !v3u8_eq(last_char->fg_col, col_c.fg_col) ||
          !v3u8_eq(last_char->bg_col, col_c.bg_col))
      {
        // TODO: Avoid snprintf.
        if(last_drawn_coords.x != i - 1 || last_drawn_coords.y != j)
        {
          // Move cursor to current position.
          if(screenbuf_size + 12 <= array_count(screenbuf))
          {
            screenbuf_size +=
              snprintf(
                  (char*)screenbuf + screenbuf_size, 13,
                  "\e[%d;%dH",
                  frame->height - j, i + 1);
          }
        }

        b32 not_drawn_yet = v2i_eq(last_drawn_coords, (v2i){0, 0});
        if(use_colors)
        {
          // TODO: Merge combined FG+BG setting into one code.
          if(use_16colors)
          {
            i32 prev_fg_16col_code = color_24bit_to_16color_code(prev_char.fg_col);
            i32 prev_bg_16col_code = color_24bit_to_16color_code(prev_char.bg_col);
            i32 curr_fg_16col_code = color_24bit_to_16color_code(col_c.fg_col);
            i32 curr_bg_16col_code = color_24bit_to_16color_code(col_c.bg_col);

            if((not_drawn_yet || prev_fg_16col_code != curr_fg_16col_code) &&
                screenbuf_size + 5 <= array_count(screenbuf))
            {
              screenbuf_size +=
                snprintf(
                    (char*)screenbuf + screenbuf_size, 6,
                    "\e[%dm",
                    30 + curr_fg_16col_code);
            }

            if((not_drawn_yet || prev_bg_16col_code != curr_bg_16col_code) &&
                screenbuf_size + 6 <= array_count(screenbuf))
            {
              screenbuf_size +=
                snprintf(
                    (char*)screenbuf + screenbuf_size, 7,
                    "\e[%dm",
                    40 + curr_bg_16col_code);
            }
          }
          else
          {
            if((not_drawn_yet || !v3u8_eq(prev_char.fg_col, col_c.fg_col)) &&
                screenbuf_size + 19 <= array_count(screenbuf))
            {
              screenbuf_size +=
                snprintf(
                    (char*)screenbuf + screenbuf_size, 20,
                    "\e[38;2;%u;%u;%um",
                    col_c.fg_col.r, col_c.fg_col.g, col_c.fg_col.b);
            }

            if((not_drawn_yet || !v3u8_eq(prev_char.bg_col, col_c.bg_col)) &&
                screenbuf_size + 19 <= array_count(screenbuf))
            {
              screenbuf_size +=
                snprintf(
                    (char*)screenbuf + screenbuf_size, 20,
                    "\e[48;2;%u;%u;%um",
                    col_c.bg_col.r, col_c.bg_col.g, col_c.bg_col.b);
            }
          }
        }
        else
        {
#if 1
          // Reverse video \e[7m if bg > fg.
          // TODO: Only emit code if (bg > fg) condition changes,
          //       not whenever fg_col or bg_col changes.
          if(not_drawn_yet ||
                !v3u8_eq(prev_char.fg_col, col_c.fg_col) ||
                !v3u8_eq(prev_char.bg_col, col_c.bg_col))
          {
            i32 fg_sum = col_c.fg_col.r + col_c.fg_col.g + col_c.fg_col.b;
            i32 bg_sum = col_c.bg_col.r + col_c.bg_col.g + col_c.bg_col.b;

            if(bg_sum > fg_sum)
            {
              if(screenbuf_size + 4 <= array_count(screenbuf))
              {
                screenbuf[screenbuf_size++] = '\e';
                screenbuf[screenbuf_size++] = '[';
                screenbuf[screenbuf_size++] = '7';
                screenbuf[screenbuf_size++] = 'm';
              }
            }
            else
            {
              if(screenbuf_size + 4 <= array_count(screenbuf))
              {
                screenbuf[screenbuf_size++] = '\e';
                screenbuf[screenbuf_size++] = '[';
                screenbuf[screenbuf_size++] = '0';
                screenbuf[screenbuf_size++] = 'm';
              }
            }
          }
#endif
        }

        if(screenbuf_size < array_count(screenbuf))
        {
          screenbuf[screenbuf_size++] = col_c.chr;
        }

        prev_char = col_c;
        last_drawn_coords = (v2i){i, j};
      }
      *last_char = col_c;
    }
  }

  if(use_colors && screenbuf_size > 0)
  {
    if(screenbuf_size + 4 <= array_count(screenbuf))
    {
      screenbuf[screenbuf_size++] = '\e';
      screenbuf[screenbuf_size++] = '[';
      screenbuf[screenbuf_size++] = '0';
      screenbuf[screenbuf_size++] = 'm';
    }
  }

  write(STDOUT_FILENO, screenbuf, screenbuf_size);

  if(draw_debug_info)
  {
    printf("\e[0m\e[%d;1H", frame->height - 2);
    printf("width  = %-5d", frame->width);
    printf("\nheight = %-5d", frame->height);
    // Show number of bytes used to draw this frame.
    printf("\nbytes output = %-6d ", screenbuf_size);
    fflush(stdout);
  }

  frame->full_redraw = false;
}

static b32 global_quitting = 0;
internal void handle_interrupt(int c)
{
  global_quitting = 1;
}

void* input_fun(void* void_data)
{
  input_thread_data_t* data = (input_thread_data_t*)void_data;
  i32 tmp_count;
  u8 tmp_buf[array_count(data->key_buf)];

  for(;;)
  {
    tmp_count = read(STDIN_FILENO, tmp_buf, array_count(tmp_buf));

    pthread_mutex_lock(&data->mutex);

    if(data->key_count + tmp_count <= array_count(data->key_buf))
    {
      for(i32 read_idx = 0;
          read_idx < tmp_count;
          ++read_idx)
      {
        data->key_buf[data->key_count] = tmp_buf[read_idx];
        ++data->key_count;
      }
    }

    pthread_mutex_unlock(&data->mutex);
  }
}

enum
{
  KEY_CTRL_A = 1,
  KEY_CTRL_B,
  KEY_CTRL_C,
  KEY_CTRL_D,
  KEY_CTRL_E,
  KEY_CTRL_F,
  KEY_CTRL_G,
  KEY_CTRL_H,
  KEY_CTRL_I,
  KEY_CTRL_J,
  KEY_CTRL_K,
  KEY_CTRL_L,
  KEY_CTRL_M,
  KEY_CTRL_N,
  KEY_CTRL_O,
  KEY_CTRL_P,
  KEY_CTRL_Q,
  KEY_CTRL_R,
  KEY_CTRL_S,
  KEY_CTRL_T,
  KEY_CTRL_U,
  KEY_CTRL_V,
  KEY_CTRL_W,
  KEY_CTRL_X,
  KEY_CTRL_Y,
  KEY_CTRL_Z,
  KEY_ESCAPE, // 27

  KEY_CTRL_BACKSPACE =   8,
  KEY_TAB            =   9,
  KEY_ENTER          =  10,
  KEY_CTRL_SLASH     =  31,
  KEY_BACKSPACE      = 127,

  // Custom
  KEY_ARROW_RIGHT = 129,
  KEY_ARROW_UP,
  KEY_ARROW_LEFT,
  KEY_ARROW_DOWN,
  KEY_CTRL_ARROW_RIGHT,
  KEY_CTRL_ARROW_UP,
  KEY_CTRL_ARROW_LEFT,
  KEY_CTRL_ARROW_DOWN,
  KEY_SHIFT_TAB,
  KEY_PAGE_DOWN,
  KEY_PAGE_UP,
  KEY_HOME,
  KEY_END,
  KEY_CTRL_HOME,
  KEY_CTRL_END,
  KEY_DELETE,
  KEY_CTRL_DELETE,
  KEY_ALT_D,
  KEY_F12,
};

internal void begin_terminal_io(terminal_context_t* ctx)
{
  *ctx = (terminal_context_t){0};

  {
    struct sigaction handle_int = {
      .sa_handler = handle_interrupt,
    };
    struct sigaction ignore = {
      .sa_handler = SIG_IGN,
    };
    sigaction(SIGINT, &handle_int, 0);  // Ctrl+C
    sigaction(SIGTERM, &handle_int, 0);  // kill
    sigaction(SIGTSTP, &ignore, 0);  // Ctrl+Z
  }

  // Turn off stdin echoing, line buffering, Ctrl+S/Q handling.
  tcgetattr(STDIN_FILENO, &ctx->termattr_orig);
  {
    struct termios termattr_new = ctx->termattr_orig;
    termattr_new.c_iflag &= ~IXON & ~IXOFF;
    termattr_new.c_lflag &= ~ECHO & ~ICANON;
    tcsetattr(STDIN_FILENO, 0, &termattr_new);
  }

  write_chars("\e[?25l");  // Hide cursor.
  // Enable mouse.
  // See: https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Mouse-Tracking
  // Use 1002h for button events, 1003h for button and motion events.
  write_chars("\e[?1002h\e[?1006h");

  // Start input thread.
  pthread_attr_t input_thread_attr;
  pthread_attr_init(&input_thread_attr);
  pthread_t input_thread;
  pthread_mutex_init(&ctx->input_thread_data.mutex, 0);
  pthread_create(&input_thread, &input_thread_attr, input_fun, &ctx->input_thread_data);

  ctx->draw_debug_info = false;
  ctx->use_colors = true;
  ctx->use_16colors = true;
}

internal void end_terminal_io(terminal_context_t* ctx)
{
  // Restore terminal state.
  write_chars("\e[?1006l\e[?1002l");  // Disable mouse.
  write_chars("\e[?25h");  // Show cursor.
  write_chars("\e[0m");  // Reset colors.
  tcsetattr(STDIN_FILENO, 0, &ctx->termattr_orig);
}

internal void get_terminal_events(terminal_context_t* ctx, live_input_t* input, char_frame_t* frame)
{
  // Resize frame.
  {
    struct winsize wins;
    ioctl(STDIN_FILENO, TIOCGWINSZ, &wins);

    if(wins.ws_col != frame->width || wins.ws_row != frame->height)
    {
      if(frame->width && frame->height)
      {
        free(frame->chars);
        free(frame->chars_last);
      }

      frame->width = wins.ws_col;
      frame->height = wins.ws_row;
      frame->pitch = wins.ws_col * sizeof(frame->chars[0]);

      frame->chars = malloc(frame->pitch * frame->height);
      frame->chars_last = malloc(frame->pitch * frame->height);
      if(!frame->chars || !frame->chars_last)
      {
        puts("Could not allocate frame buffer");
        global_quitting = 1;
      }

      frame->full_redraw = 1;
    }
  }

  *input = (live_input_t){0};
  input->mouse_pos = ctx->input_persist.mouse_pos;
  input->btn_mouse_left.ended_down = ctx->input_persist.btn_mouse_left.ended_down;
  input->btn_mouse_middle.ended_down = ctx->input_persist.btn_mouse_middle.ended_down;
  input->btn_mouse_right.ended_down = ctx->input_persist.btn_mouse_right.ended_down;
  input->modifiers_held = ctx->input_persist.modifiers_held;

  pthread_mutex_lock(&ctx->input_thread_data.mutex);

  for(i32 input_key_idx = 0;
      input_key_idx < ctx->input_thread_data.key_count;
      ++input_key_idx)
  {
    char in_char = ctx->input_thread_data.key_buf[input_key_idx];
    if(((in_char >= ' ' && in_char <= '~') ||
          (in_char >= KEY_CTRL_A && in_char <= KEY_CTRL_Z) ||
          in_char == KEY_CTRL_SLASH ||
          in_char == KEY_BACKSPACE)
        && input->typed_key_count < array_count(ctx->typed_key_buffer))
    {
      ctx->typed_key_buffer[input->typed_key_count++] = in_char;
    }

    switch(in_char)
    {
      case KEY_CTRL_D:
      {
        ctx->draw_debug_info = !ctx->draw_debug_info;
#if 0
        if(!ctx->draw_debug_info)
        {
          // Characters that had been covered by the debug info must be redrawn.
          frame->full_redraw = 1;
        }
#else
        frame->full_redraw = 1; // Get around dirty drawing
#endif
      } break;

      case KEY_CTRL_F:
      {
        if(!ctx->use_colors)
        {
          ctx->use_colors = 1;
          ctx->use_16colors = 1;
        }
        else if(ctx->use_16colors)
        {
          ctx->use_16colors = 0;
        }
        else
        {
          ctx->use_colors = 0;
        }
        frame->full_redraw = 1;
      } break;

      case KEY_CTRL_L:
      {
        frame->full_redraw = 1;
      } break;

      case KEY_ESCAPE:
      {
        b32 handled = true;
        // TODO: Check key count properly in various cases.
        if(ctx->input_thread_data.key_count - input_key_idx >= 2)
        {
          u8 next_key = ctx->input_thread_data.key_buf[input_key_idx + 1];
          char* in = (char*)ctx->input_thread_data.key_buf + input_key_idx + 2;
          input_key_idx += 1;
          if(next_key == '[')
          {
            // Some terminal code, followed by more bytes.
            if(in[0] == '<')
            {
              // Mouse input.
              u32 mouse_state;
              i32 mouse_col, mouse_row;
              char mm;
              i32 consumed_count;
              sscanf(in, "<%d;%d;%d%c%n",
                  &mouse_state, &mouse_col, &mouse_row, &mm, &consumed_count);
              u32 mouse_buttons = mouse_state & 67;
              input_key_idx += consumed_count;

              input->mouse_pos.x = mouse_col - 1;
              input->mouse_pos.y = frame->height - mouse_row;

              b32 ended_down = (mm == 'M');
              i32 transitions = !ended_down || !(mouse_state & 32) ? 1 : 0;
              if(mouse_buttons == 0)
              {
                input->btn_mouse_left.half_transitions += transitions;
                input->btn_mouse_left.ended_down = ended_down;
                ctx->input_persist.btn_mouse_left.ended_down = ended_down;
              }
              else if(mouse_buttons == 1)
              {
                input->btn_mouse_middle.half_transitions += transitions;
                input->btn_mouse_middle.ended_down = ended_down;
                ctx->input_persist.btn_mouse_middle.ended_down = ended_down;
              }
              else if(mouse_buttons == 2)
              {
                input->btn_mouse_right.half_transitions += transitions;
                input->btn_mouse_right.ended_down = ended_down;
                ctx->input_persist.btn_mouse_right.ended_down = ended_down;
              }

              if(mouse_buttons == 64)
              {
                input->mouse_scroll_y += 1;
              }
              else if(mouse_buttons == 65)
              {
                input->mouse_scroll_y -= 1;
              }

              u32 mouse_modifiers = mouse_state & 0x1c;
              input->modifiers_held =
                ((mouse_modifiers & 0x04) ? MODIFIER_SHIFT : 0) |
                ((mouse_modifiers & 0x08) ? MODIFIER_ALT   : 0) |
                ((mouse_modifiers & 0x10) ? MODIFIER_CTRL  : 0);
              ctx->input_persist.modifiers_held = input->modifiers_held;

              handled = true;
            }
            else
            {
              // TODO: Unify this with the non-"next_key == '['" case.
              struct
              {
                char* code;
                u8 key;
              } escape_code_mappings[] = {
                { "A",    KEY_ARROW_UP },
                { "B",    KEY_ARROW_DOWN },
                { "C",    KEY_ARROW_RIGHT },
                { "D",    KEY_ARROW_LEFT },
                { "F",    KEY_END },
                { "H",    KEY_HOME },
                { "Z",    KEY_SHIFT_TAB },
                { "1;5A", KEY_CTRL_ARROW_UP },
                { "1;5B", KEY_CTRL_ARROW_DOWN },
                { "1;5C", KEY_CTRL_ARROW_RIGHT },
                { "1;5D", KEY_CTRL_ARROW_LEFT },
                { "1;5F", KEY_CTRL_END },
                { "1;5H", KEY_CTRL_HOME },
                { "24~",  KEY_F12 },
                { "3~",   KEY_DELETE },
                { "3;5~", KEY_CTRL_DELETE },
                { "5~",   KEY_PAGE_UP },
                { "6~",   KEY_PAGE_DOWN },
              };

              handled = false;
              u32 code_length = 0;
              int match_key = 0;
              for(u32 mapping_idx = 0;
                  mapping_idx < array_count(escape_code_mappings) && !handled;
                  ++mapping_idx)
              {
                // TODO: Limit by input length.
                char* inc = in;
                char* c = escape_code_mappings[mapping_idx].code;
                b32 matches = true;
                while(*c && matches)
                {
                  matches = (*c == *inc);
                  ++inc;
                  ++c;
                }
                if(matches)
                {
                  handled = true;
                  code_length = (inc - in);
                  match_key = escape_code_mappings[mapping_idx].key;
                }
              }
              if(handled)
              {
                input_key_idx += code_length;
                if(input->typed_key_count < array_count(ctx->typed_key_buffer))
                {
                  ctx->typed_key_buffer[input->typed_key_count++] = match_key;
                }
              }
            }
          }
          else if(next_key == 'd')
          {
            if(input->typed_key_count < array_count(ctx->typed_key_buffer))
            {
              ctx->typed_key_buffer[input->typed_key_count++] = KEY_ALT_D;
            }
          }
          else
          {
            handled = false;
          }
        }
        else
        {
          global_quitting = true;
        }

        if(!handled)
        {
          // Avoid parsing unknown escape sequence.
          input_key_idx = ctx->input_thread_data.key_count;
        }
      } break;

      default:
      {
      } break;
    }
  }
  ctx->input_thread_data.key_count = 0;
  pthread_mutex_unlock(&ctx->input_thread_data.mutex);

  input->typed_keys = ctx->typed_key_buffer;
}
