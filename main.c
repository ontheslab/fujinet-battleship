/*
 * NABU Battleship main -- lobby, placement, and gameplay loop.
 *
 * Development stays mostly here and in the local src/nabu files so we can
 * keep shared-tree changes to a minimum while we learn where proper platform
 * hooks fit. This follows the same general single-translation-unit approach
 * used in the NABU lobby client, NABULIB-driven code is kept local and
 * pulled together from/to one main file (like lobby/adam) - avoid head explosion.
 */

#include "../misc.h"
#include "../stateclient.h"

void drawGamefieldOverlay(uint8_t quadrant, uint8_t *gamefield);
void drawIcon(uint8_t x, uint8_t y, uint8_t icon);
void drawTextColor(uint8_t x, uint8_t y, const char *s, uint8_t fg, uint8_t bg);

uint16_t nabu_fetch_url(const char *url, uint8_t *buf, uint16_t max_len); /* network.c */

#define BS_EMPTY       0x80  /* blank ocean tile */
#define BS_SUB_STERN_H 0xA8  /* submarine stern, horizontal */
#define BS_SUB_MID_H   0xA9  /* submarine middle section, horizontal */
#define BS_SUB_BOW_H   0xAA  /* submarine bow, horizontal */

/* VDP colour values used in the lobby and stage screens */
#define LC_FG_FUJINET   5   /* light blue   -- "FujiNet" */
#define LC_FG_BATTLE   11   /* light yellow -- "Battleship" / "NBATTLE" */
#define LC_FG_HEADER    7   /* cyan         -- labels and status text */
#define LC_FG_READY     3   /* light green  -- READY status */
#define LC_FG_WAIT     14   /* grey         -- wait status */
#define LC_FG_VER      14   /* grey         -- version number */
#define LC_FG_KEY       9   /* light red    -- key hints */
#define LC_BG           1   /* black background */

/* Globals the shared game code expects the platform to supply */
char serverEndpoint[50] = "https://battleship.carr-designs.com/";  /* game server base URL */
char localServer[] = "http://127.0.0.1:8080/";                     /* local testing URL */
char query[50] = "";                                               /* URL query string, built from lobby */
char playerName[12] = "";                                          /* player name from lobby */

ClientState clientState;  /* shared game and lobby state */
GameState state;          /* unused on NABU, satisfies shared build */
PrefsStruct prefs;        /* unused on NABU, satisfies shared build */

char tempBuffer[128];                         /* scratch buffer for URL building */
uint8_t shipSize[5] = {5, 4, 3, 3, 2};        /* ship lengths: carrier to destroyer */
uint8_t shipPlacements[5] = {0, 0, 0, 0, 0};  /* position of each placed ship */
uint8_t shipPlaceIndex = 0;                   /* number of ships placed so far */
uint8_t nabu_placement_preview_active;        /* preview ship visible in graphics */
uint8_t nabu_placement_preview_pos;           /* preview ship position */
static uint8_t aim_x;                         /* attack cursor column */
static uint8_t aim_y;                         /* attack cursor row */
static uint8_t lobby_ready_waiting;           /* all players ready, polling for start */
static uint8_t lobby_ready_polls;             /* polls taken since all-ready */
static uint8_t lobby_frame_drawn;             /* lobby frame drawn this display cycle */
static uint8_t lobby_sub_x;                   /* lobby submarine animation column */
static uint8_t last_raw_key;                  /* debounce: last raw scancode */
static uint8_t pending_local_attack;          /* attack in sent, result pending */
static uint8_t last_result_owner;             /* who caused last hit/miss/sunk */
static uint8_t placement_initialised;         /* manual placement set up for this turn */
static uint8_t placement_preview_pos;         /* current manual placement cursor position */
static uint8_t using_mini_lobby;              /* built-in table selector is active */
static char ui_message[31];                   /* status line message text */

/* mini-lobby: shown when no LOBBYSEL.DAT is found */
#define ML_MAX_TABLES 10

typedef struct {
    char id[16];     /* table ID for ?table= query param */
    char name[20];   /* friendly display name */
    uint8_t players; /* current connected players */
    uint8_t max;     /* max player slots */
} TableEntry;

static TableEntry ml_tables[ML_MAX_TABLES]; /* fetched table list */
static uint8_t ml_count;                    /* number of tables parsed */

static uint8_t run_mini_lobby(void);

static int read_input_key(void);
static int wait_input_key(void);
static uint8_t can_auto_place(void);
static uint8_t should_auto_poll_game(void);
static void draw_game_status_line(void);
static void set_gameplay_ui_message(void);
static void redraw_attack_cursor_only(uint8_t old_x, uint8_t old_y);
static void note_state_result_context(void);
static void set_api_fail_message(const char *path);
static uint8_t target_player_index_for(const Game *game);
static uint8_t can_attack_now_for(const Game *game);
static uint8_t redraw_gameplay_delta(const Game *old_game);
static uint8_t send_and_refresh(const char *path);
static uint8_t placement_fits_manual(uint8_t count, uint8_t size, uint8_t pos);
static uint8_t next_valid_manual_pos(uint8_t size, uint8_t start_pos);
static void ensure_manual_placement(void);
static uint8_t send_manual_placement(void);
static void redraw_manual_placement_preview(uint8_t old_preview_pos, uint8_t old_ship_index, uint8_t hide_old_preview);
static void draw_gameplay_prompt(uint8_t row);
static void draw_lobby_frame(void);
static void draw_status_block(void);
static void draw_graphics_gameplay(void);
static uint8_t return_to_lobby_after_leave(void);

#define UI_COLS                32  /* usable columns inside border */
#define UI_ROWS                24  /* usable rows inside border */
#define LOBBY_READY_POLL_PAUSE 30  /* frames between lobby polls */
#define LOBBY_READY_MAX_POLLS  12  /* poll limit */
#define RESULT_OWNER_UNKNOWN    0  /* no result owner set */
#define RESULT_OWNER_LOCAL      1  /* local player caused hit/miss/sunk */
#define RESULT_OWNER_REMOTE     2  /* opponent caused hit/miss/sunk */

/* Clear buffered keys. */
static void flush_keys(void)
{
    while (kbhit())
        cgetc();
    last_raw_key = 0;
}

/* Settle keyboard input. */
static void settle_keys(void)
{
    uint8_t i;

    for (i = 0; i < 6; ++i) {
        pause(2);
        flush_keys();
    }
}

/* Wait for a stable key state. */
static void settle_keys_long(void)
{
    uint8_t i;

    for (i = 0; i < 12; ++i) {
        pause(2);
        flush_keys();
    }
}

/* Force lowercase text. */
static void lower_text(char *s)
{
    while (*s) {
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s + 32);
        s++;
    }
}

/* Load the saved lobby selection. */
static void load_lobby_datafile(void)
{
    uint8_t i;

    read_appkey(AK_LOBBY_CREATOR_ID, AK_LOBBY_APP_ID, AK_LOBBY_KEY_USERNAME, tempBuffer);
    tempBuffer[8] = 0;
    strcpy(playerName, tempBuffer);
    lower_text(playerName);

    read_appkey(AK_LOBBY_CREATOR_ID, AK_LOBBY_APP_ID, AK_LOBBY_KEY_SERVER, tempBuffer);

    i = (uint8_t)strlen(tempBuffer);
    if (i) {
        while (--i) {
            if (tempBuffer[i] == '?') {
                strcpy(query, tempBuffer + i);
                tempBuffer[i] = 0;
                strcpy(serverEndpoint, tempBuffer);
                break;
            }
        }
    }

    if (playerName[0] && !strstr(query, "&player=")) {
        strcat(query, "&player=");
        strcat(query, playerName);
    }
}

/* Set the current UI message. */
static void set_ui_message(const char *message)
{
    strncpy(ui_message, message, sizeof(ui_message) - 1);
    ui_message[sizeof(ui_message) - 1] = 0;
}

/* Clear the current UI message. */
static void clear_ui_message(void)
{
    ui_message[0] = 0;
}

/* Draw a full status screen. */
static void draw_stage_screen(const char *title)
{
    lobby_frame_drawn = 0;
    set_ui_message(title);
    resetScreen();
    drawBox(0, 0, UI_COLS, UI_ROWS);
    drawBox(1, 1, UI_COLS - 2, 3);
    drawTextColor(2, 2, "NBATTLE", LC_FG_BATTLE, LC_BG);
    drawTextColor(23, 23, NABU_BATTLE_VERSION, LC_FG_VER, LC_BG);
    drawTextColor(2, 6, title, LC_FG_HEADER, LC_BG);
}

/* Read one local input key - attempt to update from cgetc. */
static int read_input_key(void)
{
    uint8_t pressed;
    uint8_t raw;

    /* Cloud CP/M path guided by GTAMP and Dave feedback. */
    /* __critical, port read -- 1.00.44 (from z88dk) */
    __critical {
        pressed = (uint8_t)(inp(0x91) & 0x01);
        raw = pressed ? (uint8_t)inp(0x90) : 0;
    }

    if (!pressed) {
        last_raw_key = 0;
        if (kbhit())
            return cgetc();
        return -1;
    }

    if (raw == 0x94) {
        last_raw_key = 0;
        return -1;
    }

    if (raw == last_raw_key)
        return -1;

    last_raw_key = raw;

    if (raw == 0xE2 || raw == 0xA8) return KEY_UP_ARROW;
    if (raw == 0xE3 || raw == 0xA2) return KEY_DOWN_ARROW;
    if (raw == 0xE1 || raw == 0xA1) return KEY_LEFT_ARROW;
    if (raw == 0xE0 || raw == 0xA4) return KEY_RIGHT_ARROW;
    if (raw == 0xB0) return ' ';
    if (raw == 'h' || raw == 'H') return raw;
    if (raw == 'r' || raw == 'R') return raw;
    if (raw == 'p' || raw == 'P') return raw;
    if (raw == 'q' || raw == 'Q') return raw;
    if (raw == '2' || raw == '4' || raw == '6' || raw == '8') return raw;

    if (kbhit())
        return cgetc();

    return -1;
}

/* Wait for one local input key. */
static int wait_input_key(void)
{
    int key;

    while (1) {
        key = read_input_key();
        if (key >= 0)
            return key;
        pause(1);
    }
}

static uint8_t can_attack_now(void);
static uint8_t lobby_all_ready(void);

/* Extract the current table name. */
static void copy_table_name(char *dst, uint8_t dst_len)
{
    const char *src;
    uint8_t i = 0;

    dst[0] = 0;
    src = strstr(query, "table=");
    if (!src)
        return;

    src += 6;
    while (src[i] && src[i] != '&' && i + 1 < dst_len) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

/* Return the current status text. */
static const char *turn_text(void)
{
    if (clientState.game.status == STATUS_LOBBY)
        return "lobby";

    if (clientState.game.status == STATUS_PLACE_SHIPS) {
        if (clientState.game.playerStatus == PLAYER_STATUS_PLACE_SHIPS)
            return "place ships";
        return "waiting placement";
    }

    if (clientState.game.status == STATUS_GAMEOVER)
        return "game over";

    if (can_attack_now())
        return "your turn";

    if (clientState.game.activePlayer >= 0 &&
        clientState.game.activePlayer < clientState.game.playerCount)
        return clientState.game.players[clientState.game.activePlayer].name;

    return "waiting";
}

/* Draw the top status line. */
static void draw_game_status_line(void)
{
    const char *line;

    drawSpace(0, 1, UI_COLS);

    if (ui_message[0])
        line = ui_message;
    else if (can_auto_place())
        line = "PLACE SHIPS";
    else if (can_attack_now())
        line = "YOUR TURN";
    else
        line = turn_text();

    drawText(2, 1, line);
}

/* Update the gameplay message - needs work - who won etc? */
static void set_gameplay_ui_message(void)
{
    if (clientState.game.status == STATUS_HIT) {
        set_ui_message(last_result_owner == RESULT_OWNER_LOCAL ? "YOU HIT!" :
                       last_result_owner == RESULT_OWNER_REMOTE ? "THEY HIT!" :
                       "HIT!");
        return;
    }

    if (clientState.game.status == STATUS_MISS) {
        set_ui_message(last_result_owner == RESULT_OWNER_LOCAL ? "YOU MISS" :
                       last_result_owner == RESULT_OWNER_REMOTE ? "THEY MISS" :
                       "MISS");
        return;
    }

    if (clientState.game.status == STATUS_SUNK) {
        set_ui_message(last_result_owner == RESULT_OWNER_LOCAL ? "YOU SUNK!" :
                       last_result_owner == RESULT_OWNER_REMOTE ? "THEY SUNK!" :
                       "SUNK!");
        return;
    }

    if (clientState.game.status == STATUS_GAMEOVER) {
        if (clientState.game.prompt[0])
            set_ui_message(clientState.game.prompt);
        else
            set_ui_message("GAME OVER");
        return;
    }

    if (can_auto_place()) {
        sprintf(tempBuffer, "PLACE %u/5", (unsigned)(shipPlaceIndex + 1));
        set_ui_message(tempBuffer);
        return;
    }

    if (can_attack_now()) {
        set_ui_message("YOUR TURN");
        return;
    }

    if (clientState.game.status == STATUS_GAMESTART) {
        set_ui_message("STARTING...");
        return;
    }

    if (clientState.game.status == STATUS_PLACE_SHIPS) {
        set_ui_message("WAITING PLACE");
        return;
    }

    set_ui_message(turn_text());
}

/* Record who set the last result. */
static void note_state_result_context(void)
{
    if (clientState.game.status == STATUS_HIT ||
        clientState.game.status == STATUS_MISS ||
        clientState.game.status == STATUS_SUNK) {
        if (pending_local_attack)
            last_result_owner = RESULT_OWNER_LOCAL;
        else
            last_result_owner = RESULT_OWNER_REMOTE;
    } else {
        last_result_owner = RESULT_OWNER_UNKNOWN;
    }

    pending_local_attack = 0;
}

/* Show any API error message. */
static void set_api_fail_message(const char *path)
{
    if (!path || !path[0]) {
        set_ui_message("API FAIL");
        return;
    }

    switch (path[0]) {
    case 'a':
        set_ui_message("ATTACK FAIL");
        return;
    case 'p':
        set_ui_message("PLACE FAIL");
        return;
    case 'r':
        set_ui_message("READY FAIL");
        return;
    case 's':
        set_ui_message("STATE FAIL");
        return;
    case 'l':
        set_ui_message("LEAVE FAIL");
        return;
    default:
        set_ui_message("API FAIL");
        return;
    }
}

/* From shared gameplay: check if auto-place is allowed. */
static uint8_t can_auto_place(void)
{
    return clientState.game.status == STATUS_PLACE_SHIPS &&
           clientState.game.playerStatus == PLAYER_STATUS_PLACE_SHIPS;
}

/* From shared gameplay: check if an attack is allowed. */
static uint8_t can_attack_now(void)
{
    return clientState.game.status >= STATUS_GAMESTART &&
           clientState.game.status < STATUS_GAMEOVER &&
           clientState.game.activePlayer == 0 &&
           clientState.game.playerStatus != PLAYER_STATUS_VIEWING;
}

/* From shared gameplay: test a manual ship position. */
static uint8_t placement_fits_manual(uint8_t count, uint8_t size, uint8_t pos)
{
    uint8_t occupied[100];
    uint8_t i;
    uint8_t j;
    uint8_t p;

    memset(occupied, 0, sizeof(occupied));

    for (i = 0; i < count && i < 5; ++i) {
        p = shipPlacements[i];
        for (j = 0; j < shipSize[i]; ++j) {
            if (p > 199)
                return 0;
            occupied[p % 100] = 1;
            p += (p >= 100) ? 10 : 1;
        }
    }

    for (i = 0; i < size; ++i) {
        if (occupied[pos % 100] || pos > 199 || (i > 0 && pos <= 100 && pos % 10 == 0))
            return 0;
        pos += (pos >= 100) ? 10 : 1;
    }

    return 1;
}

/* From shared gameplay: find the next valid ship position. */
static uint8_t next_valid_manual_pos(uint8_t size, uint8_t start_pos)
{
    uint16_t attempt;
    uint8_t pos = start_pos;

    for (attempt = 0; attempt < 200; ++attempt) {
        if (placement_fits_manual(shipPlaceIndex, size, pos))
            return pos;
        pos = (uint8_t)((pos + 1) % 200);
    }

    return 0;
}

/* From shared gameplay: set up manual placement state. */
static void ensure_manual_placement(void)
{
    if (!can_auto_place()) {
        placement_initialised = 0;
        nabu_placement_preview_active = 0;
        return;
    }

    if (placement_initialised)
        return;

    memset(shipPlacements, 0, sizeof(shipPlacements));
    shipPlaceIndex = 0;
    placement_preview_pos = next_valid_manual_pos(shipSize[0], 0);
    nabu_placement_preview_pos = placement_preview_pos;
    nabu_placement_preview_active = 1;
    placement_initialised = 1;
}

/* From shared gameplay: send manual ship placements. */
static uint8_t send_manual_placement(void)
{
    uint8_t i;

    strcpy(tempBuffer, "place/");
    for (i = 0; i < 5; ++i) {
        itoa(shipPlacements[i], tempBuffer + strlen(tempBuffer), 10);
        if (i < 4)
            strcat(tempBuffer, ",");
    }

    return send_and_refresh(tempBuffer);
}

/* From shared gameplay: redraw the placement preview. */
static void redraw_manual_placement_preview(uint8_t old_preview_pos, uint8_t old_ship_index, uint8_t hide_old_preview)
{
    old_preview_pos = old_preview_pos;
    old_ship_index = old_ship_index;
    hide_old_preview = hide_old_preview;
    draw_graphics_gameplay();
}

/* From shared gameplay: check if an attack is allowed for this state. */
static uint8_t can_attack_now_for(const Game *game)
{
    return game->status >= STATUS_GAMESTART &&
           game->status < STATUS_GAMEOVER &&
           game->activePlayer == 0 &&
           game->playerStatus != PLAYER_STATUS_VIEWING;
}

/* From shared gameplay: check if the game should auto-poll. */
static uint8_t should_auto_poll_game(void)
{
    if (clientState.game.status == STATUS_LOBBY)
        return 0;

    if (clientState.game.status == STATUS_GAMEOVER)
        return 0;

    if (can_auto_place())
        return 0;

    if (can_attack_now())
        return 0;

    return 1;
}

/* From shared gameplay: check if all lobby players are ready. */
static uint8_t lobby_all_ready(void)
{
    uint8_t i;

    if (clientState.game.status != STATUS_LOBBY)
        return 0;

    if (clientState.lobby.playerCount == 0)
        return 0;

    for (i = 0; i < clientState.lobby.playerCount && i < PLAYER_MAX; ++i) {
        if (clientState.lobby.players[i].ready == 0)
            return 0;
    }

    return 1;
}

/* From shared gameplay: return the current target player. */
static uint8_t target_player_index(void)
{
    uint8_t i;

    for (i = 1; i < clientState.game.playerCount && i < PLAYER_MAX; ++i) {
        if (clientState.game.players[i].playerStatus == PLAYER_STATUS_DEFAULT ||
            clientState.game.players[i].playerStatus == PLAYER_STATUS_READY) {
            return i;
        }
    }

    return 1;
}

/* From shared gameplay: return the current target player. */
static uint8_t target_player_index_for(const Game *game)
{
    uint8_t i;

    for (i = 1; i < game->playerCount && i < PLAYER_MAX; ++i) {
        if (game->players[i].playerStatus == PLAYER_STATUS_DEFAULT ||
            game->players[i].playerStatus == PLAYER_STATUS_READY) {
            return i;
        }
    }

    return 1;
}

static void draw_graphics_gameplay(void)
{
    uint8_t target;
    const char *target_name;
    uint8_t target_valid;
    uint8_t i;

    target = target_player_index();
    target_valid = (uint8_t)(target < clientState.game.playerCount && target < PLAYER_MAX);
    target_name = target_valid
        ? clientState.game.players[target].name
        : "";

    drawBoard(clientState.game.playerCount);
    draw_game_status_line();

    if (clientState.game.playerCount > 2) {
        for (i = 0; i < clientState.game.playerCount && i < PLAYER_MAX; ++i) {
            drawPlayerName(i,
                           i == 0 ? (playerName[0] ? playerName : "you")
                                  : clientState.game.players[i].name,
                           clientState.game.activePlayer == i && !(i == 0 && can_auto_place()));
            drawGamefield(i, clientState.game.players[i].gamefield);
        }

        if (clientState.game.status >= STATUS_PLACE_SHIPS &&
            clientState.game.playerStatus != PLAYER_STATUS_VIEWING) {
            if (can_auto_place() && placement_initialised) {
                for (i = 0; i < shipPlaceIndex && i < 5; ++i)
                    drawShip(0, shipSize[i], shipPlacements[i], DRAWSHIP_SHOW);
                if (shipPlaceIndex < 5)
                    drawShip(0, shipSize[shipPlaceIndex], placement_preview_pos, DRAWSHIP_SHOW);
            } else {
                for (i = 0; i < 5; ++i)
                    drawShip(0, shipSize[i], clientState.game.myShips[i], DRAWSHIP_SHOW);
            }
            drawGamefieldOverlay(0, clientState.game.players[0].gamefield);
        }

        if (clientState.game.status >= STATUS_PLACE_SHIPS) {
            for (i = 0; i < clientState.game.playerCount && i < PLAYER_MAX; ++i) {
                uint8_t j;
                for (j = 0; j < 5; ++j)
                    drawLegendShip(i, j, shipSize[j], clientState.game.players[i].shipsLeft[j]);
            }
        }

        if (can_attack_now() && target_valid)
            drawGamefieldCursor(target, aim_x, aim_y, clientState.game.players[target].gamefield, 0);

        return;
    }

    drawPlayerName(0, playerName[0] ? playerName : "you",
                   clientState.game.activePlayer == 0 && !can_auto_place());
    drawPlayerName(1, target_name, clientState.game.activePlayer == target);

    drawGamefield(0, clientState.game.players[0].gamefield);
    if (target_valid)
        drawGamefield(1, clientState.game.players[target].gamefield);

    if (clientState.game.status >= STATUS_PLACE_SHIPS &&
        clientState.game.playerStatus != PLAYER_STATUS_VIEWING) {
            if (can_auto_place() && placement_initialised) {
                for (i = 0; i < shipPlaceIndex && i < 5; ++i)
                    drawShip(0, shipSize[i], shipPlacements[i], DRAWSHIP_SHOW);
                if (shipPlaceIndex < 5)
                    drawShip(0, shipSize[shipPlaceIndex], placement_preview_pos, DRAWSHIP_SHOW);
        } else {
            for (i = 0; i < 5; ++i)
                drawShip(0, shipSize[i], clientState.game.myShips[i], DRAWSHIP_SHOW);
        }
        drawGamefieldOverlay(0, clientState.game.players[0].gamefield);
    }

    if (clientState.game.status >= STATUS_PLACE_SHIPS) {
        for (i = 0; i < 5; ++i)
            drawLegendShip(0, i, shipSize[i], clientState.game.players[0].shipsLeft[i]);
        if (target_valid) {
            for (i = 0; i < 5; ++i)
                drawLegendShip(1, i, shipSize[i], clientState.game.players[target].shipsLeft[i]);
        }
    }

    if (can_attack_now() && target_valid)
        drawGamefieldCursor(1, aim_x, aim_y, clientState.game.players[target].gamefield, 0);

    draw_gameplay_prompt(21);
}

/* Draw the gameplay controls. */
static void draw_gameplay_prompt(uint8_t row)
{
    drawSpace(0, row, UI_COLS);
    if (can_auto_place()) {
        drawTextColor(2,  row, "Spc",    LC_FG_KEY, LC_BG);
        drawText(5,       row, ":place  ");
        drawTextColor(13, row, "R",      LC_FG_KEY, LC_BG);
        drawText(14,      row, ":rotate  ");
        drawTextColor(23, row, "P",      LC_FG_KEY, LC_BG);
        drawText(24,      row, ":auto");
    } else {
        drawTextColor(2,  row, "Arrows", LC_FG_KEY, LC_BG);
        drawText(8,       row, ":aim  ");
        drawTextColor(14, row, "Spc",    LC_FG_KEY, LC_BG);
        drawText(17,      row, ":fire  ");
        drawTextColor(24, row, "Q",      LC_FG_KEY, LC_BG);
        drawText(25,      row, ":quit");
    }
}

/* Draw the lobby player list. */
static void draw_lobby_state(void)
{
    uint8_t i;
    uint8_t row;
    static char name_part[18];

    for (i = 0; i < PLAYER_MAX; ++i) {
        row = (uint8_t)(7 + i);
        if (i < clientState.lobby.playerCount) {
            sprintf(name_part, "  P%u %-10.10s ",
                    (unsigned)(i + 1),
                    clientState.lobby.players[i].name);
            drawText(2, row, name_part);
            if (clientState.lobby.players[i].ready)
                drawTextColor(18, row, "READY", LC_FG_READY, LC_BG);
            else
                drawTextColor(18, row, " wait", LC_FG_WAIT, LC_BG);
        } else {
            drawSpace(2, row, (uint8_t)(UI_COLS - 3));
        }
    }
}

/* Check for up input. */
static uint8_t is_up_key(int key)
{
    return (uint8_t)(key == KEY_UP_ARROW || key == KEY_UP_ARROW_2 || key == KEY_UP_ARROW_3 ||
                     key == '8');
}

/* Check for down input. */
static uint8_t is_down_key(int key)
{
    return (uint8_t)(key == KEY_DOWN_ARROW || key == KEY_DOWN_ARROW_2 || key == KEY_DOWN_ARROW_3 ||
                     key == '2');
}

/* Check for left input. */
static uint8_t is_left_key(int key)
{
    return (uint8_t)(key == KEY_LEFT_ARROW || key == KEY_LEFT_ARROW_2 || key == KEY_LEFT_ARROW_3 ||
                     key == '4');
}

/* Check for right input. */
static uint8_t is_right_key(int key)
{
    return (uint8_t)(key == KEY_RIGHT_ARROW || key == KEY_RIGHT_ARROW_2 || key == KEY_RIGHT_ARROW_3 ||
                     key == '6');
}

/* Draw the lobby frame. */
static void draw_lobby_frame(void)
{
    uint8_t i;

    resetScreen();
    drawBox(0, 0, UI_COLS, UI_ROWS);
    drawBox(1, 1, UI_COLS - 2, 3);
    /* "FujiNet Battleship" centered in 28-char interior box: start at 7 */
    drawTextColor(7, 2, "FujiNet", LC_FG_FUJINET, LC_BG);
    drawTextColor(14, 2, " Battleship", LC_FG_BATTLE, LC_BG);
    drawTextColor(23, 23, NABU_BATTLE_VERSION, LC_FG_VER, LC_BG);
    drawTextColor(2, 6, "Players:", LC_FG_HEADER, LC_BG);
    for (i = 1; i <= 30; ++i) {
        drawIcon(i, 12, BS_EMPTY);
        drawIcon(i, 14, BS_EMPTY);
    }
    drawTextColor(2,  21, "Spc", LC_FG_KEY, LC_BG);
    drawText(5,       21, ":ready  ");
    drawTextColor(13, 21, "R",   LC_FG_KEY, LC_BG);
    drawText(14,      21, ":refresh  ");
    drawTextColor(24, 21, "Q",   LC_FG_KEY, LC_BG);
    drawText(25,      21, ":quit");
    drawTextColor(2,  22, "H",   LC_FG_KEY, LC_BG);
    drawText(3,       22, ":in-game help");
    if (!lobby_sub_x)
        lobby_sub_x = 1;
    lobby_frame_drawn = 1;
}

/* Draw the current status block. */
static void draw_status_block(void)
{
    static char table_name[16];
    static char line[33];

    copy_table_name(table_name, sizeof(table_name));

    if (clientState.game.status == STATUS_LOBBY) {
        if (!lobby_frame_drawn)
            draw_lobby_frame();

        drawTextColor(2, 4, "Room:", LC_FG_HEADER, LC_BG);
        sprintf(line, "%-8.8s", table_name[0] ? table_name : "(none)");
        drawText(7, 4, line);
        drawTextColor(17, 4, "Count:", LC_FG_HEADER, LC_BG);
        sprintf(line, "%-2u", (unsigned)clientState.lobby.playerCount);
        drawText(23, 4, line);
        drawTextColor(2, 5, "Player:", LC_FG_HEADER, LC_BG);
        sprintf(line, "%-12.12s", playerName[0] ? playerName : "(none)");
        drawText(9, 5, line);
        draw_lobby_state();

        drawIcon(lobby_sub_x, 13, ' ');
        drawIcon((uint8_t)(lobby_sub_x + 1), 13, ' ');
        drawIcon((uint8_t)(lobby_sub_x + 2), 13, ' ');
        lobby_sub_x = (uint8_t)(lobby_sub_x < 28 ? lobby_sub_x + 1 : 1);
        drawIcon(lobby_sub_x, 13, BS_SUB_STERN_H);
        drawIcon((uint8_t)(lobby_sub_x + 1), 13, BS_SUB_MID_H);
        drawIcon((uint8_t)(lobby_sub_x + 2), 13, BS_SUB_BOW_H);
    } else {
        draw_graphics_gameplay();
    }
}

/* Show a transition snapshot. */
static void print_transition_snapshot(const char *label)
{
    if (clientState.game.status == STATUS_LOBBY)
        set_ui_message(label);
    else
        set_gameplay_ui_message();
}

/* Redraw only the attack cursor. */
static void redraw_attack_cursor_only(uint8_t old_x, uint8_t old_y)
{
    uint8_t target;

    if (!can_attack_now()) {
        draw_status_block();
        return;
    }

    target = target_player_index();
    if (target >= clientState.game.playerCount || target >= PLAYER_MAX) {
        draw_status_block();
        return;
    }

    /* quadrant from target -- 1.00.89 tester feedback - Fixed forgotten (fixed) location
       with target - Dave noted miss located hits */
    drawGamefieldUpdate(target, clientState.game.players[target].gamefield,
                        (uint8_t)(old_y * 10 + old_x), 0);
    drawGamefieldCursor(target, aim_x, aim_y, clientState.game.players[target].gamefield, 0);
}

/* redraw only changed cells -- 1.00.86 */
static uint8_t redraw_gameplay_delta(const Game *old_game)
{
    uint8_t old_target;
    uint8_t new_target;
    uint8_t i;
    uint8_t old_can_attack;
    uint8_t new_can_attack;
    uint8_t hit_changed = 0;
    uint8_t miss_changed = 0;

    if (old_game->status < STATUS_GAMESTART || clientState.game.status < STATUS_GAMESTART)
        return 0;

    if (memcmp(old_game->myShips, clientState.game.myShips, sizeof(clientState.game.myShips)) != 0)
        return 0;

    old_target = target_player_index_for(old_game);
    new_target = target_player_index();

    draw_game_status_line();

    if (clientState.game.playerCount > 2) {
        for (i = 0; i < clientState.game.playerCount && i < PLAYER_MAX; ++i) {
            drawPlayerName(i,
                           i == 0 ? (playerName[0] ? playerName : "you")
                                  : clientState.game.players[i].name,
                           clientState.game.activePlayer == i && !(i == 0 && can_auto_place()));
        }

        for (i = 0; i < clientState.game.playerCount && i < PLAYER_MAX; ++i) {
            uint8_t j;
            for (j = 0; j < 100; ++j) {
                if (old_game->players[i].gamefield[j] != clientState.game.players[i].gamefield[j]) {
                    if ((clientState.game.status == STATUS_HIT ||
                         clientState.game.status == STATUS_SUNK) &&
                        clientState.game.players[i].gamefield[j] == FIELD_ATTACK)
						{
							hit_changed = 1;
							soundHit(i);
							if (clientState.game.status == STATUS_SUNK)
								soundSunk(i);
						}
                    else if (clientState.game.status == STATUS_MISS &&
                             clientState.game.players[i].gamefield[j] == FIELD_MISS)
						{
							miss_changed = 1;
							soundMiss(i); // should all say miss or just active?
						}
                    drawGamefieldUpdate(i, clientState.game.players[i].gamefield, j, 0);
                }
            }
            for (j = 0; j < 5; ++j) {
                if (old_game->players[i].shipsLeft[j] != clientState.game.players[i].shipsLeft[j])
                    drawLegendShip(i, j, shipSize[j], clientState.game.players[i].shipsLeft[j]);
            }
        }

        old_can_attack = can_attack_now_for(old_game);
        new_can_attack = can_attack_now();

        if (old_can_attack && old_target < PLAYER_MAX)
            drawGamefieldUpdate(old_target, clientState.game.players[old_target].gamefield,
                                (uint8_t)(aim_y * 10 + aim_x), 0);
        if (new_can_attack && new_target < PLAYER_MAX)
            drawGamefieldCursor(new_target, aim_x, aim_y,
                                clientState.game.players[new_target].gamefield, 0);
    } else {
        if (old_target != new_target)
            return 0;

        drawPlayerName(0, playerName[0] ? playerName : "you",
                       clientState.game.activePlayer == 0);
        drawPlayerName(1,
                       (new_target < clientState.game.playerCount && new_target < PLAYER_MAX)
                           ? clientState.game.players[new_target].name
                           : "",
                       clientState.game.activePlayer == new_target);

        for (i = 0; i < 100; ++i) {
            if (old_game->players[0].gamefield[i] != clientState.game.players[0].gamefield[i]) {
                if ((clientState.game.status == STATUS_HIT ||
                     clientState.game.status == STATUS_SUNK) &&
                    clientState.game.players[0].gamefield[i] == FIELD_ATTACK)
					{
						hit_changed = 1;
						soundHit(0);
						if (clientState.game.status == STATUS_SUNK)
						{
							soundSunk(0);
						}
					}
                else if (clientState.game.status == STATUS_MISS &&
                         clientState.game.players[0].gamefield[i] == FIELD_MISS)
					{
						miss_changed = 1;
						soundMiss(0);
					}
                drawGamefieldUpdate(0, clientState.game.players[0].gamefield, i, 0);
            }

            if (new_target < PLAYER_MAX &&
                old_game->players[new_target].gamefield[i] != clientState.game.players[new_target].gamefield[i]) {
                if ((clientState.game.status == STATUS_HIT ||
                     clientState.game.status == STATUS_SUNK) &&
                    clientState.game.players[new_target].gamefield[i] == FIELD_ATTACK)
					{
						hit_changed = 1;
						soundHit(new_target);
						if (clientState.game.status == STATUS_SUNK)
						{
							soundSunk(new_target);
						}						
					}
                else if (clientState.game.status == STATUS_MISS &&
                         clientState.game.players[new_target].gamefield[i] == FIELD_MISS)
					{
						miss_changed = 1;
						soundMiss(new_target);
					}
                drawGamefieldUpdate(1, clientState.game.players[new_target].gamefield, i, 0);
            }
        }

        for (i = 0; i < 5; ++i) {
            if (old_game->players[0].shipsLeft[i] != clientState.game.players[0].shipsLeft[i])
                drawLegendShip(0, i, shipSize[i], clientState.game.players[0].shipsLeft[i]);

            if (new_target < PLAYER_MAX &&
                old_game->players[new_target].shipsLeft[i] != clientState.game.players[new_target].shipsLeft[i])
                drawLegendShip(1, i, shipSize[i], clientState.game.players[new_target].shipsLeft[i]);
        }

        old_can_attack = can_attack_now_for(old_game);
        new_can_attack = can_attack_now();

        if (old_can_attack)
            drawGamefieldUpdate(1, clientState.game.players[new_target].gamefield,
                                (uint8_t)(aim_y * 10 + aim_x), 0);
        if (new_can_attack)
            drawGamefieldCursor(1, aim_x, aim_y, clientState.game.players[new_target].gamefield, 0);
    }
	
    return 1;
}

/* From shared gameplay: send an action and refresh state. */
static uint8_t send_and_refresh(const char *path)
{
    uint8_t rc;

    rc = apiCall(path);
    if (rc != API_CALL_SUCCESS) {
        pending_local_attack = 0;
        set_api_fail_message(path);
        return rc;
    }

    set_ui_message(path[0] == 's' ? "STATE" :
                   path[0] == 'r' ? "READY" :
                   path[0] == 'p' ? "PLACE" :
                   path[0] == 'a' ? "ATTACK" : "API");

    if (path[0] == 'a') {
        note_state_result_context();
        set_gameplay_ui_message();
        return rc;
    }

    if (path[0] != 's') {
        rc = apiCall("state");
        if (rc != API_CALL_SUCCESS) {
            pending_local_attack = 0;
            set_ui_message("STATE FAIL");
            return rc;
        }
        note_state_result_context();
        set_gameplay_ui_message();
    }

    return rc;
}

/* Send the ready action. Uses ready/1 so NIA double-fetch just sets the same state twice. */
static uint8_t send_ready_once(void)
{
    uint8_t rc;

    rc = apiCall("ready/1");
    if (rc != API_CALL_SUCCESS) {
        set_ui_message("READY FAIL");
        return rc;
    }
    set_ui_message("READY");
    print_transition_snapshot("READY");

    return rc;
}

/* Leave the game if needed. */
static void leave_if_needed(void)
{
    if (!playerName[0] || !serverEndpoint[0])
        return;

    draw_stage_screen("Leaving...");
    if (apiCall("leave") != API_CALL_SUCCESS)
        set_ui_message("LEAVE FAIL");
    else
        set_ui_message("LEAVE");
}

/* Return to the best available lobby after leaving a live game. */
static uint8_t return_to_lobby_after_leave(void)
{
    uint8_t rc;

    leave_if_needed();
    lobby_ready_waiting = 0;
    lobby_ready_polls = 0;
    placement_initialised = 0;
    nabu_placement_preview_active = 0;
    clear_ui_message();

    if (using_mini_lobby) {
        query[0] = 0;
        settle_keys();
        if (!run_mini_lobby())
            return 0;

        draw_stage_screen("NBATTLE");
        draw_status_block();
        rc = apiCall("state");
        if (rc == API_CALL_SUCCESS) {
            note_state_result_context();
            set_ui_message("STATE");
        } else {
            set_ui_message("STATE FAIL");
        }
        draw_status_block();
        return 1;
    }

    memset(&clientState, 0, sizeof(clientState));
    clientState.lobby.status = STATUS_LOBBY;
    clientState.lobby.playerStatus = PLAYER_STATUS_DEFAULT;
    clientState.lobby.playerCount = 1;
    strncpy(clientState.lobby.players[0].name, playerName,
            sizeof(clientState.lobby.players[0].name) - 1);
    clientState.lobby.players[0].name[sizeof(clientState.lobby.players[0].name) - 1] = 0;
    clientState.lobby.players[0].ready = 0;
    set_ui_message("Left game.");
    draw_stage_screen("NBATTLE");
    draw_status_block();
    return 1;
}

/* From shared gameplay: test a ship placement. */
static uint8_t placement_fits(const uint8_t *occupied, uint8_t size, uint8_t pos)
{
    uint8_t i;

    for (i = 0; i < size; ++i) {
        if (occupied[pos % 100] || pos > 199 || (i > 0 && pos <= 100 && pos % 10 == 0))
            return 0;

        pos += (pos >= 100) ? 10 : 1;
    }

    return 1;
}

/* Mark placed ship cells. */
static void mark_placement(uint8_t *occupied, uint8_t size, uint8_t pos)
{
    uint8_t i;

    for (i = 0; i < size; ++i) {
        occupied[pos % 100] = 1;
        pos += (pos >= 100) ? 10 : 1;
    }
}

/* Auto-place all ships. */
static void auto_place_ships(void)
{
    uint8_t placements[5];
    uint8_t occupied[100];
    uint8_t i;
    uint8_t attempts;
    uint8_t pos;

    memset(occupied, 0, sizeof(occupied));

    for (i = 0; i < 5; ++i) {
        attempts = 0;
        do {
            pos = getRandomNumber(200);
            attempts++;
        } while (!placement_fits(occupied, shipSize[i], pos) && attempts != 0);

        if (!placement_fits(occupied, shipSize[i], pos)) {
            static const uint8_t fallback[5] = {0, 110, 6, 156, 83};
            memcpy(placements, fallback, sizeof(placements));
            break;
        }

        placements[i] = pos;
        mark_placement(occupied, shipSize[i], pos);
    }

    strcpy(tempBuffer, "place/");
    for (i = 0; i < 5; ++i) {
        itoa(placements[i], tempBuffer + strlen(tempBuffer), 10);
        if (i < 4)
            strcat(tempBuffer, ",");
    }
}

/* Extract a quoted string value from a JSON object slice.
   Looks for "pat":"..." and copies the value into out. */
static void ml_extract_str(const uint8_t *buf, uint16_t start, uint16_t end,
                            const char *pat, char *out, uint8_t out_max)
{
    uint8_t pat_len = (uint8_t)strlen(pat);
    uint16_t i;
    uint8_t len;

    for (i = start; i + pat_len <= end; i++) {
        if (strncmp((const char *)buf + i, pat, pat_len) == 0) {
            i += pat_len;
            len = 0;
            while (i <= end && buf[i] != '"' && len + 1 < out_max)
                out[len++] = (char)buf[i++];
            out[len] = 0;
            return;
        }
    }
    out[0] = 0;
}

/* Extract a numeric value from a JSON object slice.
   Looks for "pat":N and stores the integer in *out. */
static void ml_extract_num(const uint8_t *buf, uint16_t start, uint16_t end,
                            const char *pat, uint8_t *out)
{
    uint8_t pat_len = (uint8_t)strlen(pat);
    uint16_t i;
    uint8_t val;

    for (i = start; i + pat_len <= end; i++) {
        if (strncmp((const char *)buf + i, pat, pat_len) == 0) {
            i += pat_len;
            val = 0;
            while (i <= end && buf[i] >= '0' && buf[i] <= '9') {
                val = (uint8_t)(val * 10 + (uint8_t)(buf[i] - '0'));
                i++;
            }
            *out = val;
            return;
        }
    }
    *out = 0;
}

/* Parse the /tables JSON response into ml_tables[].
   Each entry is a {...} object with t, n, p, m fields. */
static void ml_parse_tables(const uint8_t *buf, uint16_t size)
{
    uint16_t i = 0;
    uint16_t obj_end;
    TableEntry *e;

    ml_count = 0;

    while (i < size && ml_count < ML_MAX_TABLES) {
        while (i < size && buf[i] != '{') i++;
        if (i >= size) break;

        obj_end = (uint16_t)(i + 1);
        while (obj_end < size && buf[obj_end] != '}') obj_end++;
        if (obj_end >= size) break;

        e = &ml_tables[ml_count];
        ml_extract_str(buf, i, obj_end, "\"Table\":\"",      e->id,   sizeof(e->id));
        ml_extract_str(buf, i, obj_end, "\"Name\":\"",       e->name, sizeof(e->name));
        ml_extract_num(buf, i, obj_end, "\"Players\":",      &e->players);
        ml_extract_num(buf, i, obj_end, "\"MaxPlayers\":",   &e->max);

        if (e->id[0] && e->max > 0)
            ml_count++;

        i = (uint16_t)(obj_end + 1);
    }
}

/* Draw the player name input field at row 8. */
static void ml_draw_name(void)
{
    static char field[14]; /* "> " + 8 chars + "_" + null */
    uint8_t len = (uint8_t)strlen(playerName);

    drawSpace(2, 8, 28);
    field[0] = '>';
    field[1] = ' ';
    strcpy(field + 2, playerName);
    field[2 + len] = '_';
    field[3 + len] = 0;
    drawTextColor(2, 8, field, LC_FG_READY, LC_BG);
}

static void ml_draw_row(uint8_t idx, uint8_t selected)
{
    static char line[26];
    uint8_t p = ml_tables[idx].players;
    uint8_t m = ml_tables[idx].max;

    line[0] = idx == selected ? '>' : ' ';
    line[1] = ' ';
    sprintf(line + 2, "%-18.18s", ml_tables[idx].name);
    line[20] = (p >= 10) ? (char)('0' + (p / 10) % 10) : ' ';
    line[21] = (char)('0' + p % 10);
    line[22] = '/';
    if (m >= 10) {
        line[23] = (char)('0' + (m / 10) % 10);
        line[24] = (char)('0' + m % 10);
        line[25] = 0;
    } else {
        line[23] = (char)('0' + m % 10);
        line[24] = 0;
    }
    drawText(2, (uint8_t)(8 + idx), line);
}

/* Draw the table list starting at row 8, highlighting the selected entry. */
static void ml_draw_tables(uint8_t selected)
{
    uint8_t i;

    for (i = 0; i < ml_count && i < ML_MAX_TABLES; i++)
        ml_draw_row(i, selected);
}

/* Built-in table browser and player name entry.
   Runs when no LOBBYSEL.DAT was found (playerName empty after load_lobby_datafile).
   Sets playerName and query on success. Returns 1 if the player joined, 0 if they quit. */
static uint8_t run_mini_lobby(void)
{
    static uint8_t raw_buf[512]; /* raw /tables JSON response */
    static char tbl_url[80];    /* serverEndpoint + "tables" */
    uint16_t raw_size;
    uint8_t selected = 0;
    uint8_t prev;
    int key;
    uint8_t name_len;

    /* phase 1: player name entry */
    if (!playerName[0]) {
        draw_stage_screen("Enter Player Name");
        drawTextColor(2, 10, "Return", LC_FG_KEY, LC_BG);
        drawText(8,  10, ":confirm  ");
        drawTextColor(18, 10, "Q", LC_FG_KEY, LC_BG);
        drawText(19, 10, ":quit");

        ml_draw_name();

        while (1) {
            while (!kbhit()) {}
            key = cgetc();

            if (key == 'q' || key == 'Q')
                return 0;

            if (key == KEY_RETURN) {
                if (playerName[0]) break;
                continue;
            }

            name_len = (uint8_t)strlen(playerName);
            if (key == KEY_BACKSPACE) {
                if (name_len > 0) playerName[--name_len] = 0;
            } else if (key >= ' ' && key < 127 && name_len < 8) {
                playerName[name_len] = (char)key;
                playerName[name_len + 1] = 0;
            }
            ml_draw_name();
        }
    }
    lower_text(playerName);

    /* phase 2: fetch table list */
    draw_stage_screen("Fetching Tables...");
    strcpy(tbl_url, serverEndpoint);
    strcat(tbl_url, "tables");
    raw_size = nabu_fetch_url(tbl_url, raw_buf, (uint16_t)(sizeof(raw_buf) - 1));

    if (raw_size == 0) {
        draw_stage_screen("Server Unavailable");
        drawText(2, 9,  "Could not reach server.");
        drawText(2, 11, "Press any key.");
        while (!kbhit()) {}
        cgetc();
        playerName[0] = 0;
        return 0;
    }

    raw_buf[raw_size] = 0;
    ml_parse_tables(raw_buf, raw_size);

    if (ml_count == 0) {
        draw_stage_screen("No Tables Found");
        drawText(2, 9,  "No tables available.");
        drawText(2, 11, "Press any key.");
        while (!kbhit()) {}
        cgetc();
        playerName[0] = 0;
        return 0;
    }

    /* phase 3: table selection */
    draw_stage_screen("Select a Table");
    drawTextColor(4,  7, "Table", LC_FG_HEADER, LC_BG);
    drawTextColor(24, 7, "In/Of", LC_FG_HEADER, LC_BG);
    ml_draw_tables(0);
    drawTextColor(2,  19, "Up/Dn", LC_FG_KEY, LC_BG);
    drawText(7,       19, ":pick  ");
    drawTextColor(14, 19, "Spc", LC_FG_KEY, LC_BG);
    drawText(17,      19, ":join  ");
    drawTextColor(24, 19, "Q", LC_FG_KEY, LC_BG);
    drawText(25,      19, ":quit");

    while (1) {
        key = wait_input_key();

        if (key == 'q' || key == 'Q') {
            playerName[0] = 0;
            return 0;
        }

        if (is_up_key(key)) {
            prev = selected;
            selected = (uint8_t)((selected + ml_count - 1) % ml_count);
            ml_draw_row(prev, selected);
            ml_draw_row(selected, selected);
        } else if (is_down_key(key)) {
            prev = selected;
            selected = (uint8_t)((selected + 1) % ml_count);
            ml_draw_row(prev, selected);
            ml_draw_row(selected, selected);
        } else if (key == KEY_SPACEBAR || key == KEY_RETURN) {
            strcpy(query, "?table=");
            strcat(query, ml_tables[selected].id);
            strcat(query, "&player=");
            strcat(query, playerName);
            return 1;
        }
    }
}

/* Main program */
/* Run the NABU client. */
void main(void)
{
    uint8_t rc;
    int key;
    Game prev_game;

    load_lobby_datafile();
    initGraphics();

    /* if no LOBBYSEL.DAT was found, run the built-in table browser */
    if (!playerName[0]) {
        using_mini_lobby = 1;
        if (!run_mini_lobby())
            return;
    }

    draw_stage_screen("NBATTLE");
    draw_status_block();
    memcpy(&prev_game, &clientState.game, sizeof(Game));
    rc = apiCall("state");
    if (rc == API_CALL_SUCCESS) {
        note_state_result_context();
        set_ui_message("STATE");
    } else {
        set_ui_message("STATE FAIL");
    }
    draw_status_block();

    while (1) {
        uint8_t moved = 0;
        uint8_t old_aim_x = aim_x;
        uint8_t old_aim_y = aim_y;

        if (!lobby_ready_waiting && lobby_all_ready()) {
            lobby_ready_waiting = 1;
            lobby_ready_polls = 0;
            set_ui_message("All ready.");
            draw_status_block();
        }

        if (lobby_ready_waiting && clientState.game.status == STATUS_LOBBY) {
            if (kbhit()) {
                key = cgetc();
                if (key == 'q' || key == 'Q') {
                    flush_keys();
                    leave_if_needed();
                    settle_keys();
                    break;
                }
                if (key == 'r' || key == 'R') {
                    lobby_ready_waiting = 0;
                    flush_keys();
                    draw_stage_screen("Fetching...");
                    memcpy(&prev_game, &clientState.game, sizeof(Game));
                    rc = apiCall("state");
                    if (rc == API_CALL_SUCCESS) {
                        note_state_result_context();
                        set_ui_message("STATE");
                        print_transition_snapshot("STATE");
                        if (!redraw_gameplay_delta(&prev_game))
                            draw_status_block();
                    } else {
                        set_ui_message("STATE FAIL");
                        draw_status_block();
                    }
                    settle_keys();
                    continue;
                }
            }

            pause(LOBBY_READY_POLL_PAUSE);
            memcpy(&prev_game, &clientState.game, sizeof(Game));
            rc = apiCall("state");
            if (rc == API_CALL_SUCCESS) {
                note_state_result_context();
                sprintf(tempBuffer, "STATE%u", (unsigned)(lobby_ready_polls + 1));
                set_ui_message(tempBuffer);
                print_transition_snapshot("STATE");
                if (!redraw_gameplay_delta(&prev_game))
                    draw_status_block();
            } else {
                set_ui_message("STATE FAIL");
                draw_status_block();
            }

            if (rc != API_CALL_SUCCESS) {
                lobby_ready_waiting = 0;
                settle_keys();
                continue;
            }

            if (clientState.game.status != STATUS_LOBBY) {
                lobby_ready_waiting = 0;
                settle_keys();
                continue;
            }

            lobby_ready_polls++;
            if (lobby_ready_polls >= LOBBY_READY_MAX_POLLS) {
                lobby_ready_waiting = 0;
                set_ui_message("Still waiting.");
                draw_status_block();
                settle_keys();
                continue;
            }
            continue;
        }

        if (should_auto_poll_game()) {
            /* Q during opponent turn -- 1.00.89 tester feedback */
            {
                int pk = read_input_key();
                if (pk == 'q' || pk == 'Q') {
                    if (!return_to_lobby_after_leave())
                        break;
                    settle_keys();
                    continue;
                }
            }
            pause(LOBBY_READY_POLL_PAUSE);
            if (clientState.game.status == STATUS_GAMESTART)
                set_ui_message("Starting...");
            else if (clientState.game.status == STATUS_PLACE_SHIPS)
                set_ui_message("Wait place...");
            else
                set_ui_message("Wait turn...");
            draw_game_status_line();
            memcpy(&prev_game, &clientState.game, sizeof(Game));
            rc = apiCall("state");
            if (rc == API_CALL_SUCCESS) {
                note_state_result_context();
                clear_ui_message();
                print_transition_snapshot("STATE");
                if (!redraw_gameplay_delta(&prev_game))
                    draw_status_block();
            } else {
                set_ui_message("STATE FAIL");
                draw_status_block();
            }
            settle_keys();
            continue;
        }

        ensure_manual_placement();

        if (clientState.game.status == STATUS_LOBBY) {
            while (!kbhit())
                pause(1);
            key = cgetc();
        } else {
            key = wait_input_key();
        }

        if (clientState.game.status == STATUS_GAMEOVER) {
            if (key == 'q' || key == 'Q') {
                flush_keys();
                if (!return_to_lobby_after_leave())
                    break;
                settle_keys();
                continue;
            }

            if (key == 'r' || key == 'R') {
                flush_keys();
                set_ui_message("Fetching...");
                draw_game_status_line();
                memcpy(&prev_game, &clientState.game, sizeof(Game));
                rc = apiCall("state");
                if (rc == API_CALL_SUCCESS) {
                    note_state_result_context();
                    clear_ui_message();
                    print_transition_snapshot("STATE");
                    if (!redraw_gameplay_delta(&prev_game))
                        draw_status_block();
                } else {
                    set_ui_message("STATE FAIL");
                    draw_status_block();
                }
                settle_keys();
                continue;
            }

            if (key == 'h' || key == 'H') {
                flush_keys();
                draw_gameplay_prompt(1);
                settle_keys_long();
                continue;
            }

            flush_keys();
            continue;
        }

        if (key == 'q' || key == 'Q') {
            flush_keys();
            if (clientState.game.status != STATUS_LOBBY) {
                if (!return_to_lobby_after_leave())
                    break;
                settle_keys();
                continue;
            }
            leave_if_needed();
            settle_keys();
            break;
        }

        if (can_auto_place() && (key == 'r' || key == 'R')) {
            uint8_t size;
            uint8_t next;
            uint8_t old_preview_pos = placement_preview_pos;
            uint8_t old_ship_index = shipPlaceIndex;

            flush_keys();
            size = shipSize[shipPlaceIndex];
            next = (uint8_t)((placement_preview_pos >= 100 ? placement_preview_pos - 100 : placement_preview_pos + 100));
            placement_preview_pos = next_valid_manual_pos(size, next);
            clear_ui_message();
            nabu_placement_preview_pos = placement_preview_pos;
            redraw_manual_placement_preview(old_preview_pos, old_ship_index, 1);
            settle_keys();
            continue;
        }

        if (key == 'r' || key == 'R') {
            flush_keys();
            if (clientState.game.status == STATUS_LOBBY)
                draw_stage_screen("Fetching...");
            else {
                set_ui_message("Fetching...");
                draw_game_status_line();
            }
            memcpy(&prev_game, &clientState.game, sizeof(Game));
            rc = apiCall("state");
            if (rc == API_CALL_SUCCESS) {
                note_state_result_context();
                clear_ui_message();
                print_transition_snapshot("STATE");
                if (!redraw_gameplay_delta(&prev_game))
                    draw_status_block();
            } else {
                set_ui_message("STATE FAIL");
                draw_status_block();
            }
            settle_keys();
            continue;
        }

        if (key == 'h' || key == 'H') {
            flush_keys();
            if (clientState.game.status == STATUS_LOBBY) {
                set_ui_message("Spc:ready  R:refresh  Q:quit");
                draw_status_block();
            } else {
                draw_gameplay_prompt(1);
            }
            settle_keys_long();
            continue;
        }

        if (key == 'p' || key == 'P') {
            flush_keys();
            if (clientState.game.status == STATUS_LOBBY)
                draw_stage_screen("Auto-placing...");
            else {
                set_ui_message("Auto-placing...");
                draw_game_status_line();
            }
            if (can_auto_place()) {
                auto_place_ships();
                memcpy(&prev_game, &clientState.game, sizeof(Game));
                rc = send_and_refresh(tempBuffer);
                (void)rc;
                if (rc == API_CALL_SUCCESS) {
                    clear_ui_message();
                    if (!redraw_gameplay_delta(&prev_game))
                        draw_status_block();
                } else {
                    draw_status_block();
                }
                settle_keys_long();
            } else {
                settle_keys();
            }
            continue;
        }

        if (can_auto_place() && shipPlaceIndex < 5 &&
            (is_up_key(key) || is_down_key(key) || is_left_key(key) || is_right_key(key))) {
            uint8_t size = shipSize[shipPlaceIndex];
            uint8_t x = (uint8_t)(placement_preview_pos % 10);
            uint8_t y = (uint8_t)((placement_preview_pos % 100) / 10);
            uint8_t vertical = (uint8_t)(placement_preview_pos >= 100);
            uint8_t maxW = (uint8_t)(11 - (vertical ? 1 : size));
            uint8_t maxH = (uint8_t)(11 - (vertical ? size : 1));
            uint8_t old_preview_pos = placement_preview_pos;
            uint8_t old_ship_index = shipPlaceIndex;

            flush_keys();
            if (is_left_key(key))
                x = (uint8_t)((x + maxW - 1) % maxW);
            else if (is_right_key(key))
                x = (uint8_t)((x + 1) % maxW);
            else if (is_up_key(key))
                y = (uint8_t)((y + maxH - 1) % maxH);
            else if (is_down_key(key))
                y = (uint8_t)((y + 1) % maxH);

            placement_preview_pos = (uint8_t)(y * 10 + x + (vertical ? 100 : 0));
            nabu_placement_preview_pos = placement_preview_pos;
            clear_ui_message();
            redraw_manual_placement_preview(old_preview_pos, old_ship_index, 1);
            settle_keys();
            continue;
        }

        if (is_up_key(key)) {
            aim_y = (uint8_t)((aim_y + 9) % 10);
            moved = 1;
        } else if (is_down_key(key)) {
            aim_y = (uint8_t)((aim_y + 1) % 10);
            moved = 1;
        } else if (is_left_key(key)) {
            aim_x = (uint8_t)((aim_x + 9) % 10);
            moved = 1;
        } else if (is_right_key(key)) {
            aim_x = (uint8_t)((aim_x + 1) % 10);
            moved = 1;
        } else if (key == ' ') {
            flush_keys();
            if (clientState.game.status == STATUS_LOBBY) {
                draw_stage_screen("Sending ready...");
                rc = send_ready_once();
                (void)rc;
                draw_status_block();
                set_ui_message("Ready sent.");
                draw_status_block();
                lobby_ready_waiting = 1;
                lobby_ready_polls = 0;
                settle_keys_long();
                continue;
            }

            if (can_auto_place() && shipPlaceIndex < 5) {
                uint8_t old_preview_pos = placement_preview_pos;
                uint8_t old_ship_index = shipPlaceIndex;

                flush_keys();
                if (placement_fits_manual(shipPlaceIndex, shipSize[shipPlaceIndex], placement_preview_pos)) {
                    shipPlacements[shipPlaceIndex] = placement_preview_pos;
                    shipPlaceIndex++;
                    if (shipPlaceIndex < 5) {
                        placement_preview_pos = next_valid_manual_pos(shipSize[shipPlaceIndex], 0);
                        nabu_placement_preview_pos = placement_preview_pos;
                        clear_ui_message();
                        redraw_manual_placement_preview(old_preview_pos, old_ship_index, 0);
                    } else {
                        nabu_placement_preview_active = 0;
                        memcpy(&prev_game, &clientState.game, sizeof(Game));
                        rc = send_manual_placement();
                        if (rc == API_CALL_SUCCESS) {
                            placement_initialised = 0;
                            clear_ui_message();
                            if (!redraw_gameplay_delta(&prev_game))
                                draw_status_block();
                        } else {
                            draw_status_block();
                        }
                    }
                } else {
                    set_ui_message("PLACE FAIL");
                    draw_status_block();
                }
                settle_keys();
                continue;
            }

            if (can_attack_now()) {
                pending_local_attack = 1;
                soundAttack();
                set_ui_message("Sending attack...");
                draw_game_status_line();
                strcpy(tempBuffer, "attack/");
                itoa((uint8_t)(aim_y * 10 + aim_x), tempBuffer + strlen(tempBuffer), 10);
                memcpy(&prev_game, &clientState.game, sizeof(Game));
                rc = send_and_refresh(tempBuffer);
                (void)rc;
                if (rc == API_CALL_SUCCESS) {
                    if (!redraw_gameplay_delta(&prev_game))
                        draw_status_block();
                } else {
                    draw_status_block();
                }
                settle_keys();
                continue;
            }
        }

        if (moved) {
            flush_keys();
            redraw_attack_cursor_only(old_aim_x, old_aim_y);
            settle_keys();
            continue;
        }

    }
}

#include "screen.c"
#include "network.c"
#include "graphics.c"
#include "input.c"
#include "sound.c"
#include "util.c"
