// Emacs style mode select   -*- C -*-

//=======================
// Design notes for this port:
//   - Screenshot capture is delegated to `grim`, the standard wlroots
//     screencopy CLI tool, invoked via popen() and piped straight into
//     memory (no temp files). This avoids reimplementing the
//     wlr-screencopy-unstable-v1 Wayland protocol by hand in C, which
//     `libwayshot` did for the Rust version. `grim` already exists on
//     virtually every sway install and does exactly that.
//   - Monitor/output geometry (position + logical size, which matters for
//     HiDPI scaled outputs) is read from `swaymsg -t get_outputs -r`,
//     parsed with a small hand-rolled JSON scanner (no libjson dependency).
//   - Everything else (camera pan/zoom inertia, spotlight shader, flashlight
//     radius control, mirror mode) is a straight translation of the
//     original main.rs logic.
//
// Build: see build.sh / Makefile in this directory.
//========================

// _POSIX_C_SOURCE must be defined before any system headers are included
// (popen/pclose are POSIX, not part of strict C11) so glibc exposes them.
#define _POSIX_C_SOURCE 200809L

#include <raylib.h>
#include <rlgl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <unistd.h>

#define SPOTLIGHT_TINT ((Color){0x00, 0x00, 0x00, 190})
#define VELOCITY_THRESHOLD 15.0f

//--------
// Output (monitor) description, as read from `swaymsg -t get_outputs -r`
//-------
typedef struct
{
    char name[128];
    int  x, y;           // logical position
    int  width, height;  // logical size (post-scaling, matches grim/raylib coords)
    bool active;
} Output;

typedef struct
{
    Output   *items;
    int       count;
} OutputList;

static void
free_outputs(OutputList *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

// -----------------------
// Tiny helpers to run a command and capture its stdout into a heap buffer.
// Used both for `swaymsg` (text/JSON) and `grim` (binary PNG data).
// -----------------------
static unsigned char
*run_command_capture(const char *cmd, size_t *out_len)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 1 << 20; // 1 MiB initial buffer, grows as needed
    size_t len = 0;
    unsigned char *buf = malloc(cap);
    
    if (!buf)
    { pclose(fp); return NULL; }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, fp)) > 0)
    {
        len += n;
        if (len == cap)
	{
            cap *= 2;
            unsigned char *nbuf = realloc(buf, cap);
            if (!nbuf)
	    { free(buf); pclose(fp); return NULL; }

            buf = nbuf;
        }
    }

    int status = pclose(fp);
    if (status != 0)
    {
        free(buf);
        return NULL;
    }

    *out_len = len;
    return buf;
}

// ------------------
// Minimal JSON scanning helpers (no external dependency).
// We only need to pull a handful of scalar fields out of `swaymsg`
// output, so a full parser would be overkill. These helpers scan for
// "key": value pairs within a bounded string range (one output object
// at a time).
// ------------------

// memmem isn't part of strict C (it's a GNU/BSD extension); provide a
// trivial fallback so this builds portably even without _GNU_SOURCE.

static const char *memmem_fallback
( const char *haystack,
  size_t hlen,
  const char *needle,
  size_t nlen )
{
    if (nlen == 0 || hlen < nlen) return NULL;
    
    for (size_t i = 0; i + nlen <= hlen; i++)
    {
        if (memcmp(haystack + i, needle, nlen) == 0)
	    return haystack + i;
    }
    return NULL;
}

static const char *find_key
( const char *start,
  const char *end,
  const char *key )
{
    char pattern[160];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t patlen = strlen(pattern);
    
    return memmem_fallback(start, (size_t)(end - start), pattern, patlen);
}

static bool json_get_int_field
( const char *obj_start,
  const char *obj_end,
  const char *key,
  long *out )
{
    const char *hit = find_key(obj_start, obj_end, key);
    if (!hit)
	return false;
    const char *p = hit + strlen(key) + 2; // skip past "key"

    while (p < obj_end && (*p == ':' || *p == ' ' || *p == '\t'))
	p++;

    char *endp;
    long v = strtol(p, &endp, 10);
    if (endp == p)
	return false;
    *out = v;
    return true;
}

static bool json_get_bool_field
( const char *obj_start,
  const char *obj_end,
  const char *key,
  bool *out )
{
    const char *hit = find_key(obj_start, obj_end, key);
    if (!hit)
	return false;
    const char *p = hit + strlen(key) + 2;
    
    while (p < obj_end && (*p == ':' || *p == ' ' || *p == '\t'))
	p++;
    if (strncmp(p, "true", 4) == 0)
    {
	*out = true;
	return true;
    }
    
    if (strncmp(p, "false", 5) == 0)
    {
	*out = false;
	return true;
    }
    
    return false;
}

static bool json_get_string_field
( const char *obj_start,
  const char *obj_end,
  const char *key,
  char *out,
  size_t outsz )
{
    const char *hit = find_key(obj_start, obj_end, key);
    if (!hit)
	return false;
    const char *p = hit + strlen(key) + 2;
    while (p < obj_end && (*p == ':' || *p == ' ' || *p == '\t'))
	p++;
    if (*p != '"')
	return false;
    p++;
    
    size_t i = 0;
    while (p < obj_end && *p != '"' && i + 1 < outsz)
    {
        out[i++] = *p++;
    }
    
    out[i] = '\0';
    return true;
}

// Splits the top-level JSON array returned by `swaymsg -t get_outputs -r`
// into per-object [start,end) spans, one per output. Assumes well-formed,
// non-nested-in-weird-ways JSON, which swaymsg reliably produces.
static void split_json_objects
( const char *json,
  size_t len,
  const char ***starts,
  const char ***ends,
  int *count )
{
    int cap = 8;
    const char **s = malloc(sizeof(char *) * cap);
    const char **e = malloc(sizeof(char *) * cap);
    int n = 0;

    int depth = 0;
    const char *obj_start = NULL;
    for (size_t i = 0; i < len; i++)
    {
        char c = json[i];
        if (c == '{')
	{
            if (depth == 0) obj_start = &json[i];
            depth++;
        }
	else if (c == '}')
	{
            depth--;
            if (depth == 0 && obj_start)
	    {
                if (n == cap)
		{
                    cap *= 2;
                    s = realloc(s, sizeof(char *) * cap);
                    e = realloc(e, sizeof(char *) * cap);
                }
                s[n] = obj_start;
                e[n] = &json[i] + 1;
                n++;
                obj_start = NULL;
            }
        }
    }

    *starts = s;
    *ends = e;
    *count = n;
}

//////////////////////////////////////////////////////////////////////////
// Parse `swaymsg -t get_outputs -r` JSON into an OutputList.	        //
// Each output object looks roughly like:			        //
// {								        //
//   "name": "DP-1",						        //
//   "active": true,						        //
//   "rect": {"x":0,"y":0,"width":2560,"height":1440},		        //
//   ...							        //
// }								        //
// We use "rect" (the logical/layout geometry sway itself uses, already //
// scale-adjusted) rather than "current_mode" (physical pixel mode),    //
// because that's what matches grim's screenshot pixel dimensions.      //
//////////////////////////////////////////////////////////////////////////

static bool parse_sway_outputs
( const char *json,
	size_t len,
	OutputList *out )
{
    const char **starts, **ends;
    int count;
    split_json_objects(json, len, &starts, &ends, &count);
    if (count == 0)
    {
        free(starts);
        free(ends);
        return false;
    }

    Output *items = calloc((size_t)count, sizeof(Output));
    int n = 0;

    for (int i = 0; i < count; i++)
    {
        const char *os = starts[i];
        const char *oe = ends[i];

        Output o = {0};
        if (!json_get_string_field(os, oe, "name", o.name, sizeof(o.name)))
	    continue;

        bool active = true;
        json_get_bool_field(os, oe, "active", &active);
        o.active = active;

        // Find the "rect" sub-object and read x/y/width/height from within it.
        const char *rect_hit = find_key(os, oe, "rect");
        const char *rect_start = os, *rect_end = oe;

        if (rect_hit)
	{
            // narrow the search range to the rect object's braces
            const char *p = rect_hit;
	    
            while (p < oe && *p != '{') p++;
            if (p < oe)
	    {
                int depth = 0;
                const char *rs = p;
                for (; p < oe; p++)
		{
                    if (*p == '{')
			depth++;
                    else if (*p == '}')
		    {
                        depth--;
                        if (depth == 0)
			{
			    rect_start = rs;
			    rect_end = p + 1;
			    break;
			}
                    }
                }
            }
        }

        long x = 0, y = 0, w = 0, h = 0;
        json_get_int_field(rect_start, rect_end, "x", &x);
        json_get_int_field(rect_start, rect_end, "y", &y);
        json_get_int_field(rect_start, rect_end, "width", &w);
        json_get_int_field(rect_start, rect_end, "height", &h);

        o.x = (int)x;
        o.y = (int)y;
        o.width = (int)w;
        o.height = (int)h;

        if (o.width > 0 && o.height > 0)
	{
            items[n++] = o;
        }
    }

    free(starts);
    free(ends);

    if (n == 0)
    {
        free(items);
        return false;
    }

    out->items = items;
    out->count = n;
    return true;
}

static bool
get_sway_outputs(OutputList *out)
{
    size_t len;
    unsigned char *buf = run_command_capture("swaymsg -t get_outputs -r 2>/dev/null", &len);
    if (!buf) return false;
    bool ok = parse_sway_outputs((const char *)buf, len, out);
    free(buf);
    return ok;
}

// ------------
// Screenshot capture via grim
// ------------

// Shell-quote a string for safe interpolation into a popen() command line
// (wraps in single quotes, escaping embedded single quotes).
static void shell_quote
( const char *in,
 char *out,
 size_t outsz )
{
    size_t oi = 0;
    if (oi + 1 < outsz)
	out[oi++] = '\'';
    for (const char *p = in; *p && oi + 5 < outsz; p++)
    {
        if (*p == '\'')
	{
            out[oi++] = '\'';
            out[oi++] = '\\';
            out[oi++] = '\'';
            out[oi++] = '\'';
        }
	else
	{
            out[oi++] = *p;
        }
    }
    if (oi + 1 < outsz)
	out[oi++] = '\'';
    
    out[oi] = '\0';
}

// Captures a screenshot with grim (either a single named output, or the
// whole screen if output_name is NULL) and loads it as a raylib Image.
// Returns true on success and fills *out_image.
static bool capture_screenshot
( const char *output_name,
        bool show_cursor,
        Image *out_image)
{
    char cmd[512];
    char quoted[192];

    if (output_name)
    {
        shell_quote(output_name, quoted, sizeof(quoted));
        snprintf(cmd, sizeof(cmd), "grim %s -t png -o %s - 2>/dev/null",
                 show_cursor ? "-c" : "", quoted);
    }
    else
    {
        snprintf
	    (cmd,
	     sizeof(cmd), "grim %s -t png - 2>/dev/null",
                 show_cursor ? "-c" : ""
	    );
    }

    size_t len;
    unsigned char *png_data = run_command_capture(cmd, &len);
    if (!png_data || len == 0)
    {
        free(png_data);
        return false;
    }

    Image img = LoadImageFromMemory(".png", png_data, (int)len);
    free(png_data);

    if (img.data == NULL)
	return false;

    if (img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
    {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    }

    *out_image = img;
    return true;
}

// -----------------
// CLI argument handling
// -----------------
typedef struct
{
    const char *monitor_name; // window is shown on this output
    const char *output_name;  // this output is captured (NULL = all)
    float radius_multiplier;
    bool show_cursor;
} Args;

static void
print_help_and_exit(const char *bin)
{
    fprintf(stderr,
        "%s - Wayland screen-zoom tool\n"
        "\n"
        "USAGE:\n"
        "    %s [OPTIONS]\n"
        "\n"
        "OPTIONS:\n"
        "    --monitor <name>     Monitor that the window will be shown on. Defaults to primary if not specified\n"
        "    --output <name>      Monitor that should be captured. Captures all monitors if not specified\n"
        "    --radius <number>    Size of the vignette radius. Bigger number means bigger radius. Defaults to 1\n"
        "    -S --show-cursor     Whether to show cursor. Defaults to false, specify to enable\n",
        bin, bin);
    exit(0);
}

static Args
parse_args(int argc, char **argv)
{
    Args args = {0};
    args.radius_multiplier = 1.0f;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--monitor") == 0)
	{
            if (i + 1 >= argc)
	    {
		fprintf(stderr, "--monitor needs a value\n");
		exit(1);
	    }
            args.monitor_name = argv[++i];
        }
	else if (strcmp(argv[i], "--output") == 0)
	{
            if (i + 1 >= argc)
	    {
		fprintf(stderr, "--output needs a value\n");
		exit(1);
	    }
            args.output_name = argv[++i];
        }
	else if (strcmp(argv[i], "--radius") == 0)
	{
            if (i + 1 >= argc)
	    {
		fprintf(stderr, "--radius needs a value\n");
		exit(1);
	    }
            char *endp;
            float r = strtof(argv[++i], &endp);
            if (endp == argv[i])
	    {
		fprintf(stderr, "--radius must be a valid number (e.g., 5 or 3.5)\n");
		exit(1);
	    }
            args.radius_multiplier = r;
        }
	else if (strcmp(argv[i], "--show-cursor") == 0 || strcmp(argv[i], "-S") == 0)
	{
            args.show_cursor = true;
        }
	else
	{
            print_help_and_exit(argv[0]);
        }
    }
    return args;
}

// -------
// main
// -------
int
main(int argc, char **argv)
{
    Args args = parse_args(argc, argv);

    OutputList outputs = {0};
    if (!get_sway_outputs(&outputs) || outputs.count == 0)
    {
        fprintf(stderr, "No Wayland outputs found (is swaymsg available and are you running under sway?).\n");
        return 1;
    }

    // Resolve which output the window should appear on.
    int display_idx = 0;
    if (args.monitor_name)
    {
        bool found = false;
        for (int i = 0; i < outputs.count; i++)
	{
            if (strcmp(outputs.items[i].name, args.monitor_name) == 0)
	    {
                display_idx = i;
                found = true;
                break;
            }
        }
        if (!found)
	{
            fprintf(stderr, "Monitor '%s' not found.\n", args.monitor_name);
            return 1;
        }
    }
    Output *display_output = &outputs.items[display_idx];

    // Resolve which output should be captured (NULL = capture everything).
    Output *selected_output = NULL;
    if (args.output_name)
    {
        bool found = false;
        for (int i = 0; i < outputs.count; i++)
	{
            if (strcmp(outputs.items[i].name, args.output_name) == 0)
	    {
                selected_output = &outputs.items[i];
                found = true;
                break;
            }
        }
        if (!found)
	{
            fprintf(stderr, "Output '%s' not found.\n", args.output_name);
            return 1;
        }
    }

    // Take the screenshot BEFORE opening our own window, so our
    // undecorated fullscreen window doesn't end up in its own capture.
    Image screenshot_image;
    if (!capture_screenshot
	(selected_output ? selected_output->name : NULL,
         args.show_cursor, &screenshot_image))
	
    {
        fprintf(stderr, "Failed to take a screenshot (is `grim` installed?).\n");
        return 1;
    }
    
//>>>>>>>>
    // raylib window setup //
//>>>>>>>>
    SetConfigFlags(FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TRANSPARENT);
    InitWindow(display_output->width, display_output->height, "woomer");
    SetWindowState(FLAG_VSYNC_HINT);

    // Move the window onto the requested monitor, then fullscreen it there.
    // raylib enumerates monitors itself (via GLFW), so we match by matching
    // index order against sway's output list as a best effort: most sway
    // setups have raylib/GLFW enumerate monitors in the same order sway
    // reports them. If your setup differs, use --monitor to pick explicitly
    // and verify placement; SetWindowMonitor is still called defensively.
    int rl_monitor_count = GetMonitorCount();
    int rl_monitor_idx = display_idx < rl_monitor_count ? display_idx : 0;
    
    SetWindowMonitor(rl_monitor_idx);
    ToggleFullscreen();

    Texture2D screenshot_texture = LoadTextureFromImage(screenshot_image);
    UnloadImage(screenshot_image);

    // Try to load the shader from the usual relative location first
    // (next to the executable, as installed), falling back to a compiled-in
    // copy so the binary still works if the shaders/ dir isn't present.

    Shader spotlight_shader;
    {
        const char *candidates[] =
        {
            "shaders/spotlight.fs",
            "/usr/share/woomer/shaders/spotlight.fs",
            "/usr/local/share/woomer/shaders/spotlight.fs",
        };
        bool loaded = false;
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
	{
            if (FileExists(candidates[i]))
	    {
                spotlight_shader = LoadShader(NULL, candidates[i]);
                loaded = true;
                break;
            }
        }
        if (!loaded)
	{
            static const char *fallback_fs =
                "#version 330\n"
                "in vec2 fragTexCoord;\n"
                "in vec4 fragColor;\n"
                "uniform sampler2D texture0;\n"
                "uniform vec4 colDiffuse;\n"
                "uniform vec4 spotlightTint;\n"
                "uniform vec2 cursorPosition;\n"
                "uniform float spotlightRadiusMultiplier;\n"
                "const int UNIT_RADIUS = 60;\n"
                "out vec4 finalColor;\n"
                "void main() {\n"
                "    vec4 texelColor = texture(texture0, fragTexCoord);\n"
                "    float distanceToCursor = distance(gl_FragCoord.xy, vec2(cursorPosition.x, cursorPosition.y));\n"
                "    float spotlightRadius = float(UNIT_RADIUS) * spotlightRadiusMultiplier;\n"
                "    if (distanceToCursor > spotlightRadius) {\n"
                "        finalColor = (mix(texelColor, vec4(spotlightTint.rgb, 1.0), spotlightTint.a) * colDiffuse);\n"
                "    } else {\n"
                "        finalColor = (texelColor * colDiffuse);\n"
                "    }\n"
                "}\n";
            spotlight_shader = LoadShaderFromMemory(NULL, fallback_fs);
        }
    }

    int spotlight_tint_loc = GetShaderLocation(spotlight_shader, "spotlightTint");
    int cursor_position_loc = GetShaderLocation(spotlight_shader, "cursorPosition");
    int spotlight_radius_multiplier_loc = GetShaderLocation(spotlight_shader, "spotlightRadiusMultiplier");

    Camera2D camera = {0};
    camera.zoom = 1.0f;
    camera.target = (Vector2){ (float)display_output->x, (float)display_output->y };
    camera.offset = (Vector2){ 0, 0 };
    camera.rotation = 0.0f;

    double delta_scale = 0.0;
    Vector2 scale_pivot = GetMousePosition();
    Vector2 velocity = { 0, 0 };
    float spotlight_radius_multiplier = 1.0f;
    double spotlight_radius_multiplier_delta = 0.0;
    bool mirror = false;
    bool enable_spotlight = false;
    bool should_exit = false;

    Vector2 prev_mouse_pos = GetMousePosition();

    while (!WindowShouldClose() && !should_exit)
    {
        float dt = GetFrameTime();

        // Q or A to quit (covers AZERTY/QWERTY layouts, matching upstream)
        if (IsKeyPressed(KEY_Q) || IsKeyPressed(KEY_A))
	{
            should_exit = true;
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            break;
        }

        // Ctrl: momentary "flashlight burst" — jump the radius up, then let it
        // decay back down over time via spotlight_radius_multiplier_delta.
        if (IsKeyPressed(KEY_LEFT_CONTROL) || IsKeyPressed(KEY_RIGHT_CONTROL))
	{
            spotlight_radius_multiplier = args.radius_multiplier * 3.0f;
            spotlight_radius_multiplier_delta = -15.0;
        }
        if (IsKeyPressed(KEY_F))
	{
            enable_spotlight = !enable_spotlight;
        }

        float scrolled_amount = GetMouseWheelMoveV().y;
        if (scrolled_amount != 0.0f)
	{
            bool shift_down = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            if (!shift_down)
	    {
                delta_scale += (double)scrolled_amount;
            }
	    else if (enable_spotlight && shift_down)
	    {
                // Shift+Scroll: directly resize the flashlight radius, proportional
                // to its current size so each tick feels like a consistent zoom step
                // whether the circle is tiny or huge. This is a direct, persistent
                // change (not a decaying impulse like zoom/the Ctrl burst above),
                // so the new size sticks until you scroll again.
                float step = 1.0f + 0.15f * scrolled_amount;
                spotlight_radius_multiplier =
		    (float)fmin(fmax(
                    (double)(spotlight_radius_multiplier * step), 0.3), 10.0);
                // Cancel any in-flight Ctrl-burst decay so it doesn't immediately
                // undo the resize you just asked for.
                spotlight_radius_multiplier_delta = 0.0;
            }
            scale_pivot = GetMousePosition();
        }

        if (fabs(delta_scale) > 0.5)
	{
            Vector2 p0 = { scale_pivot.x / camera.zoom, scale_pivot.y / camera.zoom };
            camera.zoom = (float)fmin(fmax((double)camera.zoom + delta_scale * (double)dt, 0.01), 100000.0);
            Vector2 p1 = { scale_pivot.x / camera.zoom, scale_pivot.y / camera.zoom };
            camera.target.x += p0.x - p1.x;
            camera.target.y += p0.y - p1.y;
            delta_scale -= delta_scale * (double)dt * 4.0;
        }

        // Apply any pending decay (currently only set by the Ctrl burst above).
        if (spotlight_radius_multiplier_delta != 0.0)
	{
            spotlight_radius_multiplier = (float)fmin(fmax(
                (double)spotlight_radius_multiplier + spotlight_radius_multiplier_delta * (double)dt,
                0.3), 10.0);
            spotlight_radius_multiplier_delta -= spotlight_radius_multiplier_delta * (double)dt * 8.0;
        }

        Vector2 mouse_pos = GetMousePosition();
        Vector2 mouse_delta = { mouse_pos.x - prev_mouse_pos.x, mouse_pos.y - prev_mouse_pos.y };
        prev_mouse_pos = mouse_pos;

        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
	{
            Vector2 prev_screen_pos = { mouse_pos.x - mouse_delta.x, mouse_pos.y - mouse_delta.y };
            Vector2 world_prev = GetScreenToWorld2D(prev_screen_pos, camera);
            Vector2 world_now = GetScreenToWorld2D(mouse_pos, camera);
            Vector2 delta = { world_prev.x - world_now.x, world_prev.y - world_now.y };
            camera.target.x += delta.x;
            camera.target.y += delta.y;
            float fps = (float)GetFPS();
            velocity.x = delta.x * fps;
            velocity.y = delta.y * fps;
        }
	else if (
	    (velocity.x * velocity.x + velocity.y * velocity.y) > VELOCITY_THRESHOLD * VELOCITY_THRESHOLD)
	{
            camera.target.x += velocity.x * dt;
            camera.target.y += velocity.y * dt;
            float damp = fminf(fmaxf(camera.zoom, 0.6f), 10.0f);
            velocity.x -= velocity.x * dt * 6.0f * damp;
            velocity.y -= velocity.y * dt * 6.0f * damp;
        }

        if (IsKeyPressed(KEY_ZERO))
	{
            camera.zoom = 1.0f;
            camera.target = (Vector2)
		{
                selected_output ? (float)selected_output->x : 0.0f,
                selected_output ? (float)selected_output->y : 0.0f
                };
            mirror = false;
        }

        if (IsKeyPressed(KEY_M))
	{
            mirror = !mirror;
        }

        BeginDrawing();
        BeginMode2D(camera);

        if (enable_spotlight)
	{
            ClearBackground(SPOTLIGHT_TINT);
            Vector2 mouse_world_screen = GetMousePosition(); // spotlight uses raw screen-space cursor (matches gl_FragCoord)
            float tint_normalized[4] =
	    {
                SPOTLIGHT_TINT.r / 255.0f,
                SPOTLIGHT_TINT.g / 255.0f,
                SPOTLIGHT_TINT.b / 255.0f,
                SPOTLIGHT_TINT.a / 255.0f
            };
            SetShaderValue(spotlight_shader, spotlight_tint_loc, tint_normalized, SHADER_UNIFORM_VEC4);

            float screen_height = (float)GetScreenHeight();
            float cursor_pos[2] = { mouse_world_screen.x, screen_height - mouse_world_screen.y };
            SetShaderValue(spotlight_shader, cursor_position_loc, cursor_pos, SHADER_UNIFORM_VEC2);

            float radius_val = spotlight_radius_multiplier * camera.zoom;
            SetShaderValue(spotlight_shader, spotlight_radius_multiplier_loc, &radius_val, SHADER_UNIFORM_FLOAT);

            BeginShaderMode(spotlight_shader);
            DrawTexture(screenshot_texture, 0, 0, WHITE);
            EndShaderMode();
        }
	else
	{
            ClearBackground(BLANK);
            float scr_w = (float)GetScreenWidth();
            float scr_h = (float)GetScreenHeight();
            Rectangle src = { 0, 0, mirror ? -scr_w : scr_w, scr_h };
            Rectangle dst = { 0, 0, scr_w, scr_h };
            DrawTexturePro(screenshot_texture, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        }

        EndMode2D();
        EndDrawing();
    }

    UnloadShader(spotlight_shader);
    UnloadTexture(screenshot_texture);
    CloseWindow();
    free_outputs(&outputs);

    return 0;
}
