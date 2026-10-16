#include "ui_asteroids.h"
#include "ui_scale.h"
#include "palette.h"
#include "ui_nav.h"
#include "screen_sleep.h"
#include "../input/haptics.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

// Everything here is worked in panel pixels (not the 240 grid) as floats,
// with pxf() applied to the tuning constants so the game plays the same on
// the CrowPanel and the Knob. Angles are degrees, 0 = straight up,
// clockwise positive -- the way the knob turns.
namespace
{
    enum class State : uint8_t { Playing, Confirm, Over };

    const int ROCK_VERTS = 9;

    struct Rock
    {
        bool alive = false;
        uint8_t size = 0; // 0 small, 1 medium, 2 large
        float x = 0, y = 0, vx = 0, vy = 0;
        float ang = 0, spin = 0;
        float shape[ROCK_VERTS] = {0};
    };

    struct Bullet
    {
        bool alive = false;
        float x = 0, y = 0, vx = 0, vy = 0;
        float lifeMs = 0;
    };

    const int MAX_ROCKS = 32;   // 6 large rocks' worth of fragments, at the wave cap
    const int MAX_BULLETS = 6;
    const int MAX_WAVE_ROCKS = 6;
    const int START_LIVES = 3;

    const float TURN_PER_DETENT = 10.0f; // degrees
    const uint32_t FRAME_MS = 33;
    const uint32_t FIRE_GAP_MS = 180;
    const float BULLET_LIFE_MS = 1100;
    const uint32_t INVULN_MS = 2000;
    const uint32_t RESTART_LOCKOUT_MS = 800; // a frantic tap shouldn't skip the game-over screen

    // A very weak pull toward the ship, in px/s^2, so a rock's path curves
    // a little toward the middle over many seconds -- enough that near
    // misses eventually become hits. The cap stops a long fall from
    // building into something unplayable.
    const float PULL = 1.6f;
    const float MAX_ROCK_SPEED = 75.0f;

    const int SCORE_BY_SIZE[3] = {100, 50, 20};

    lv_obj_t *screen = nullptr;
    lv_obj_t *canvas = nullptr;
    lv_color_t *canvasBuf = nullptr;
    lv_obj_t *scoreLbl = nullptr;
    lv_obj_t *msgLbl = nullptr;
    lv_obj_t *confirmBox = nullptr;
    lv_timer_t *timer = nullptr;

    Rock rocks[MAX_ROCKS];
    Bullet bullets[MAX_BULLETS];

    State state = State::Playing;
    State confirmFrom = State::Playing; // what the quit prompt returns to
    float shipAng = 0;
    int lives = START_LIVES;
    int score = 0;
    int wave = 0;
    uint32_t lastTickAt = 0;
    uint32_t lastFireAt = 0;
    uint32_t invulnUntil = 0;
    uint32_t overAt = 0;

    float centre() { return PANEL_RES / 2.0f; }
    float rockRadius(int size) { return pxf(size == 2 ? 22.0f : size == 1 ? 12.0f : 6.0f); }
    float shipRadius() { return pxf(6.0f); }

    float frand(float lo, float hi) { return lo + (hi - lo) * (random(0, 10001) / 10000.0f); }
    float rad(float deg) { return deg * (float)M_PI / 180.0f; }

    // ---------------------------------------------------------------- rocks

    Rock *freeRock()
    {
        for (Rock &r : rocks)
            if (!r.alive) return &r;
        return nullptr;
    }

    // Small rocks are quicker, so they are harder to hit as well as smaller.
    float rockSpeed(int size) { return pxf(frand(16.0f, 34.0f)) * (1.0f + (2 - size) * 0.35f); }

    void shapeRock(Rock &r)
    {
        for (int i = 0; i < ROCK_VERTS; i++) r.shape[i] = frand(0.7f, 1.15f);
        r.ang = frand(0, 360);
        r.spin = frand(-40.0f, 40.0f);
    }

    // Scattered across the field at a random heading, but never close to
    // the ship, so a new wave can't land on top of it and the rocks aren't
    // all arriving from the rim at the same moment.
    void spawnRock(int size)
    {
        Rock *r = freeRock();
        if (!r) return;
        float a = rad(frand(0, 360));
        float dist = frand(pxf(55.0f), centre() * 0.95f);
        float h = rad(frand(0, 360));
        float sp = rockSpeed(size);
        r->alive = true;
        r->size = size;
        r->x = centre() + sinf(a) * dist;
        r->y = centre() - cosf(a) * dist;
        r->vx = cosf(h) * sp;
        r->vy = sinf(h) * sp;
        shapeRock(*r);
    }

    void startWave()
    {
        wave++;
        int n = 2 + wave;
        if (n > MAX_WAVE_ROCKS) n = MAX_WAVE_ROCKS;
        for (int i = 0; i < n; i++) spawnRock(2);
    }

    // A destroyed rock leaves two of the next size down, thrown off at
    // right angles-ish to where it was heading.
    //
    // Takes the parent by value: it is usually the rock that was just
    // marked dead, which freeRock() will hand straight back as the first
    // fragment's slot -- overwriting the parent half-way through the split.
    void splitRock(Rock parent)
    {
        if (parent.size == 0) return;
        for (int i = 0; i < 2; i++)
        {
            Rock *r = freeRock();
            if (!r) return;
            r->alive = true;
            r->size = parent.size - 1;
            r->x = parent.x;
            r->y = parent.y;
            float heading = atan2f(parent.vy, parent.vx) + rad(i == 0 ? frand(30, 90) : -frand(30, 90));
            float sp = rockSpeed(r->size);
            r->vx = cosf(heading) * sp;
            r->vy = sinf(heading) * sp;
            shapeRock(*r);
        }
    }

    bool anyRocks()
    {
        for (const Rock &r : rocks)
            if (r.alive) return true;
        return false;
    }

    // ---------------------------------------------------------------- game

    void newGame()
    {
        for (Rock &r : rocks) r.alive = false;
        for (Bullet &b : bullets) b.alive = false;
        state = State::Playing;
        shipAng = 0;
        lives = START_LIVES;
        score = 0;
        wave = 0;
        lastTickAt = millis();
        invulnUntil = millis() + INVULN_MS;
        startWave();
        lv_obj_add_flag(msgLbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(confirmBox, LV_OBJ_FLAG_HIDDEN);
    }

    void fire()
    {
        uint32_t now = millis();
        if (state != State::Playing || now - lastFireAt < FIRE_GAP_MS) return;
        for (Bullet &b : bullets)
        {
            if (b.alive) continue;
            float a = rad(shipAng);
            float speed = pxf(170.0f);
            float nose = pxf(9.0f);
            b.alive = true;
            b.x = centre() + sinf(a) * nose;
            b.y = centre() - cosf(a) * nose;
            b.vx = sinf(a) * speed;
            b.vy = -cosf(a) * speed;
            b.lifeMs = BULLET_LIFE_MS;
            lastFireAt = now;
            return;
        }
    }

    void showGameOver()
    {
        state = State::Over;
        overAt = millis();
        char buf[64];
        snprintf(buf, sizeof(buf), "GAME OVER\n%d\ntouch to play again", score);
        lv_label_set_text(msgLbl, buf);
        lv_obj_clear_flag(msgLbl, LV_OBJ_FLAG_HIDDEN);
    }

    void loseLife()
    {
        Haptics::detent();
        if (--lives <= 0)
        {
            lives = 0;
            showGameOver();
            return;
        }
        invulnUntil = millis() + INVULN_MS;
    }

    void step(float dt)
    {
        float c = centre();

        for (Rock &r : rocks)
        {
            if (!r.alive) continue;
            float gx = c - r.x, gy = c - r.y;
            float gd = sqrtf(gx * gx + gy * gy);
            if (gd > 1.0f)
            {
                r.vx += gx / gd * pxf(PULL) * dt;
                r.vy += gy / gd * pxf(PULL) * dt;
            }
            float sp = sqrtf(r.vx * r.vx + r.vy * r.vy), maxSp = pxf(MAX_ROCK_SPEED);
            if (sp > maxSp)
            {
                r.vx *= maxSp / sp;
                r.vy *= maxSp / sp;
            }
            r.x += r.vx * dt;
            r.y += r.vy * dt;
            r.ang += r.spin * dt;
            // Off the edge of the glass: come back in at the opposite edge
            // on the same heading. Mirrored through the centre, a rock that
            // was leaving is then heading back toward the middle.
            float dx = r.x - c, dy = r.y - c;
            float lim = c + rockRadius(r.size);
            if (dx * dx + dy * dy > lim * lim && dx * r.vx + dy * r.vy > 0)
            {
                float d = sqrtf(dx * dx + dy * dy);
                r.x = c - dx / d * lim;
                r.y = c - dy / d * lim;
            }
        }

        for (Bullet &b : bullets)
        {
            if (!b.alive) continue;
            b.x += b.vx * dt;
            b.y += b.vy * dt;
            b.lifeMs -= dt * 1000.0f;
            float dx = b.x - c, dy = b.y - c;
            if (b.lifeMs <= 0 || dx * dx + dy * dy > c * c) b.alive = false;
        }

        // Bullets against rocks.
        for (Bullet &b : bullets)
        {
            if (!b.alive) continue;
            for (Rock &r : rocks)
            {
                if (!r.alive) continue;
                float dx = b.x - r.x, dy = b.y - r.y;
                float hit = rockRadius(r.size) + pxf(1.0f);
                if (dx * dx + dy * dy > hit * hit) continue;
                b.alive = false;
                r.alive = false;
                score += SCORE_BY_SIZE[r.size];
                splitRock(r);
                break;
            }
        }

        // Rocks against the ship.
        if (millis() >= invulnUntil)
        {
            for (Rock &r : rocks)
            {
                if (!r.alive) continue;
                float dx = r.x - c, dy = r.y - c;
                float hit = rockRadius(r.size) + shipRadius();
                if (dx * dx + dy * dy > hit * hit) continue;
                r.alive = false;
                splitRock(r);
                loseLife();
                break;
            }
        }

        if (state == State::Playing && !anyRocks()) startWave();
    }

    // ---------------------------------------------------------------- drawing

    lv_draw_line_dsc_t lineStyle(lv_color_t color)
    {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = color;
        d.width = px(1);
        d.round_start = d.round_end = 1;
        return d;
    }

    // Draws a closed outline from `n` points given relative to (cx, cy),
    // rotated by `deg`.
    void drawPoly(const float (*pts)[2], int n, float cx, float cy, float deg, lv_draw_line_dsc_t &d)
    {
        lv_point_t out[ROCK_VERTS + 1];
        float s = sinf(rad(deg)), co = cosf(rad(deg));
        for (int i = 0; i <= n; i++)
        {
            const float *p = pts[i % n];
            out[i].x = (lv_coord_t)lroundf(cx + p[0] * co - p[1] * s);
            out[i].y = (lv_coord_t)lroundf(cy + p[0] * s + p[1] * co);
        }
        lv_canvas_draw_line(canvas, out, n + 1, &d);
    }

    void drawShip(float cx, float cy, float deg, lv_draw_line_dsc_t &d)
    {
        const float pts[4][2] = {
            {0, -pxf(9.0f)},
            {pxf(6.0f), pxf(7.0f)},
            {0, pxf(4.0f)},
            {-pxf(6.0f), pxf(7.0f)},
        };
        drawPoly(pts, 4, cx, cy, deg, d);
    }

    void draw()
    {
        lv_canvas_fill_bg(canvas, Palette::bgApp(), LV_OPA_COVER);

        lv_draw_line_dsc_t rockStyle = lineStyle(Palette::border());
        for (const Rock &r : rocks)
        {
            if (!r.alive) continue;
            float pts[ROCK_VERTS][2];
            float rr = rockRadius(r.size);
            for (int i = 0; i < ROCK_VERTS; i++)
            {
                float a = rad(360.0f * i / ROCK_VERTS);
                pts[i][0] = sinf(a) * rr * r.shape[i];
                pts[i][1] = -cosf(a) * rr * r.shape[i];
            }
            drawPoly(pts, ROCK_VERTS, r.x, r.y, r.ang, rockStyle);
        }

        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = Palette::accent();
        dot.bg_opa = LV_OPA_COVER;
        dot.radius = LV_RADIUS_CIRCLE;
        lv_coord_t bs = px(3);
        for (const Bullet &b : bullets)
            if (b.alive) lv_canvas_draw_rect(canvas, (lv_coord_t)lroundf(b.x) - bs / 2, (lv_coord_t)lroundf(b.y) - bs / 2, bs, bs, &dot);

        // Blinks while protected after a hit, so you can tell it's safe.
        bool blinkOff = millis() < invulnUntil && ((millis() / 120) & 1);
        if (state != State::Over && !blinkOff)
        {
            lv_draw_line_dsc_t shipStyle = lineStyle(Palette::text());
            drawShip(centre(), centre(), shipAng, shipStyle);
        }

        // Spare lives, as little ships along the bottom.
        lv_draw_line_dsc_t lifeStyle = lineStyle(Palette::textMuted());
        for (int i = 0; i < lives; i++)
        {
            const float pts[4][2] = {
                {0, -pxf(5.0f)}, {pxf(3.5f), pxf(4.0f)}, {0, pxf(2.0f)}, {-pxf(3.5f), pxf(4.0f)}};
            drawPoly(pts, 4, centre() + (i - (lives - 1) / 2.0f) * pxf(14.0f), PANEL_RES - pxf(24.0f), 0, lifeStyle);
        }

        char buf[16];
        snprintf(buf, sizeof(buf), "%d", score);
        lv_label_set_text(scoreLbl, buf);
        lv_obj_invalidate(canvas);
    }

    void tick(lv_timer_t *)
    {
        if (!screen || lv_scr_act() != screen || !canvas) return;
        uint32_t now = millis();
        // Held still while the quit prompt is up; and don't let the time
        // spent there arrive as one enormous step afterwards.
        if (state == State::Confirm)
        {
            lastTickAt = now;
            return;
        }
        ScreenSleep::keepAwake(); // a game is being played, however little the knob moves
        float dt = (now - lastTickAt) / 1000.0f;
        lastTickAt = now;
        if (dt > 0.06f) dt = 0.06f;
        if (state == State::Playing) step(dt);
        draw();
    }

    // ---------------------------------------------------------------- prompt

    void openConfirm()
    {
        if (state == State::Confirm) return;
        lv_obj_clear_flag(confirmBox, LV_OBJ_FLAG_HIDDEN);
        confirmFrom = state;
        state = State::Confirm;
    }

    void keepPlaying()
    {
        if (state != State::Confirm) return;
        lv_obj_add_flag(confirmBox, LV_OBJ_FLAG_HIDDEN);
        state = confirmFrom;
        lastTickAt = millis();
    }

    void canvasCb(lv_event_t *e)
    {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_PRESSED)
        {
            if (state == State::Playing) fire();
            else if (state == State::Over && millis() - overAt > RESTART_LOCKOUT_MS) newGame();
        }
        else if (code == LV_EVENT_LONG_PRESSED)
        {
            openConfirm();
        }
    }

    void keepCb(lv_event_t *) { keepPlaying(); }
    void quitCb(lv_event_t *) { UiNav::goHome(); }

    lv_obj_t *makeConfirmButton(lv_obj_t *parent, const char *text, lv_color_t bg, lv_coord_t y, lv_event_cb_t cb)
    {
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_set_size(btn, px(120), px(30));
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, y);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, &UI_FONT_14, 0);
        lv_obj_set_style_text_color(lbl, Palette::accentFg(), 0);
        lv_obj_center(lbl);
        return btn;
    }
}

lv_obj_t *uiAsteroidsCreate()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, Palette::bgApp(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // The whole glass is the playfield (and the fire button).
    canvasBuf = (lv_color_t *)heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(PANEL_RES, PANEL_RES), MALLOC_CAP_SPIRAM);
    if (canvasBuf)
    {
        canvas = lv_canvas_create(screen);
        lv_canvas_set_buffer(canvas, canvasBuf, PANEL_RES, PANEL_RES, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(canvas, canvasCb, LV_EVENT_ALL, nullptr);
    }

    scoreLbl = lv_label_create(screen);
    lv_obj_set_style_text_font(scoreLbl, &UI_FONT_14, 0);
    lv_obj_set_style_text_color(scoreLbl, Palette::text(), 0);
    lv_obj_clear_flag(scoreLbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(scoreLbl, LV_ALIGN_TOP_MID, 0, px(22));

    msgLbl = lv_label_create(screen);
    lv_obj_set_style_text_font(msgLbl, &UI_FONT_14, 0);
    lv_obj_set_style_text_color(msgLbl, Palette::text(), 0);
    lv_obj_set_style_text_align(msgLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(msgLbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(msgLbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(msgLbl, LV_OBJ_FLAG_HIDDEN);

    // The quit prompt: a scrim that swallows touches (so nothing fires
    // behind it) with the two answers on top.
    confirmBox = lv_obj_create(screen);
    lv_obj_remove_style_all(confirmBox);
    lv_obj_set_size(confirmBox, PANEL_RES, PANEL_RES);
    lv_obj_center(confirmBox);
    lv_obj_set_style_bg_color(confirmBox, Palette::bgApp(), 0);
    lv_obj_set_style_bg_opa(confirmBox, LV_OPA_90, 0);
    lv_obj_add_flag(confirmBox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *q = lv_label_create(confirmBox);
    lv_label_set_text(q, "Quit game?");
    lv_obj_set_style_text_font(q, &UI_FONT_16, 0);
    lv_obj_set_style_text_color(q, Palette::text(), 0);
    lv_obj_align(q, LV_ALIGN_CENTER, 0, px(-42));
    makeConfirmButton(confirmBox, "Keep playing", Palette::accent(), px(-2), keepCb);
    makeConfirmButton(confirmBox, "Quit", Palette::bgSecondary(), px(38), quitCb);
    lv_obj_add_flag(confirmBox, LV_OBJ_FLAG_HIDDEN);

    timer = lv_timer_create(tick, FRAME_MS, nullptr);
    newGame();
    return screen;
}

void uiAsteroidsOnShow()
{
    if (screen) newGame();
}

void uiAsteroidsHandleRotate(int32_t delta)
{
    if (state == State::Playing) shipAng = fmodf(shipAng + delta * TURN_PER_DETENT + 3600.0f, 360.0f);
}

void uiAsteroidsHandleClick()
{
    if (state == State::Playing) fire();
    else if (state == State::Confirm) keepPlaying();
    else if (millis() - overAt > RESTART_LOCKOUT_MS) newGame();
}

void uiAsteroidsHandleLongPress()
{
    if (state == State::Confirm) UiNav::goHome();
    else openConfirm();
}
