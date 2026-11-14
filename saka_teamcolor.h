/* The team color fit cheat's state for the F2 팀컬러 tab (saka_hooks.c writes it, saka_overlay.cpp shows it). */
#ifndef SAKA_TEAMCOLOR_H
#define SAKA_TEAMCOLOR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int on, mode;                    /* mode 0 자동, 1 지정 */
    uint32_t pick[3];                /* 지정: the colors asked for */
    uint32_t level;                  /* the skill level given with a style, 1-7 */
    int loaded;                      /* a game is loaded and its eleven could be read */
    uint32_t active[3];              /* what the game makes of the club now (0 = none) */
    uint8_t active_level[3];
    uint32_t plain[3];               /* and what it would be with the players' own styles */
    uint8_t plain_level[3];
    uint32_t target[3];              /* the colors being fitted */
    uint8_t target_state[3];         /* 0 fitted; 1 no starters at its positions; 2 tactic values; 3 policies; 4 the
                                        KEY player's skill; 5 not used by the game; 6 holds, but three stronger
                                        colors come first */
    int ntarget, nforced;
    struct { uint32_t id, natural, style; } forced[12];   /* the starters whose style is changed */
    uint32_t leader_id, leader_orig; /* the KEY player (role list 5) and the original skill given to him (0 = none) */
    uint8_t tac[5];                  /* the formation's five tactic values, 0-4 */
    uint8_t color_tac[71][10];       /* per color: the five (min, max) tactic ranges, from the game's table */
    uint8_t color_unused[71];        /* 1 = the game does not try this color for the club now */
    int upper;                       /* the 18 unused "upper" colors are open to the user's club */
} saka_tc_status;
void saka_teamcolor_status(saka_tc_status* out);
#ifdef __cplusplus
}
#endif
#endif
