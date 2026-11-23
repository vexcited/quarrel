#ifndef QUARREL_DISCORD_ETF_H
#define QUARREL_DISCORD_ETF_H

#include <stddef.h>
#include <stdint.h>

/* ========================================================================== */
/* erlang's external term format, the way discord's erlpack speaks it. the    */
/* rest of the code only knows json, so a term is turned into json text as    */
/* erlpack turns it into a javascript value, and back.                        */
/*                                                                            */
/*   nil, null        null              a binary       a string               */
/*   true, false      a boolean         a list, tuple  an array               */
/*   another atom     a string          a map          an object              */
/*   an integer of 4 bytes or less      a number                              */
/*   a wider integer, up to 8 bytes     a string of its decimal digits        */
/* ========================================================================== */

// one term, with its version byte, as nul terminated json from malloc.
// false when the term is malformed, `json` is then left alone.
[[nodiscard]] bool etf_to_json(const uint8_t *etf, size_t len, char **json,
                               size_t *json_len);

// one json value as a term from malloc. false when `json` is no json.
[[nodiscard]] bool etf_from_json(const char *json, size_t len, uint8_t **etf,
                                 size_t *etf_len);

#endif
