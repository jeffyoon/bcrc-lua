/*
Copyright (c) 2010 Wurldtech Security Technologies.

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

/*-
-- bcrc - binding to boost/crc, a generic CRC library.

For details on the meaning of bcrc.new() arguments, see the boost documentation
at <http://www.boost.org/doc/libs/1_34_1/libs/crc/crc.html>.

The builtin CRC types such as bcrc.crc16() use specialized implementations that
may have higher performance than those created by bcrc.new().

Also, note that process_bit() isn't supported by the optimal implementations,
which makes it a bit harder to support, so I only supported byte-wise CRCs.

Parameterizations of a number of CRC algorithms are described at
<http://regregex.bbcmicro.net/crc-catalogue.htm>. The relationship
between the catalogue's parameter names and the bcrc.new() arguments
are:

    Width  -> bits
    Poly   -> poly
    Init   -> initial
    XorOut -> xor
    RefIn  -> reflect_input
    RefOut -> reflect_output
*/

// TODO - could using lua_pushnumber()/luaL_check|optnumber()/lua_Number
// preserve checksums for widths higher than those expressable as an int32?

#include <boost/crc.hpp>
#include <inttypes.h>

/*
CRC wrapper, implementing a CRC interface. This is a work-around for the
templatization of boost/crc, which creates a different type per CRC width.
*/
class Crc
{
    public:
        virtual ~Crc() {};
        virtual void reset() = 0;
        virtual void process_bytes(const void* buffer, size_t byte_count) = 0;
        virtual uintmax_t checksum() const = 0;
};

template < std::size_t Bits >
class CrcBasic : public Crc
{
    private:

        boost::crc_basic<Bits> crc_;

    public:

        CrcBasic(
                 int truncated_polynominal,
                 int initial_remainder,
                 int final_xor_value,
                 bool reflect_input,
                 bool reflect_remainder
            ) : crc_(
                 truncated_polynominal,
                 initial_remainder,
                 final_xor_value,
                 reflect_input,
                 reflect_remainder
            )
        {
        }

        ~CrcBasic() {};

        void reset()
        {
            crc_.reset();
        }

        void process_bytes(const void* buffer, size_t byte_count)
        {
            crc_.process_bytes(buffer, byte_count);
        }

        uintmax_t checksum() const
        {
            return crc_.checksum();
        }
};

template < class Optimal >
class CrcOptimal : public Crc
{
    private:

        Optimal crc_;

    public:

        ~CrcOptimal() {};

        void reset()
        {
            crc_.reset();
        }

        void process_bytes(const void* buffer, size_t byte_count)
        {
            crc_.process_bytes(buffer, byte_count);
        }

        uintmax_t checksum() const
        {
            return crc_.checksum();
        }
};

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#undef LUALIB_API
#define LUALIB_API extern "C"

static void v_obj_metatable(lua_State* L, const char* regid, const struct luaL_reg methods[])
{
    /* metatable = { ... methods ... } */
    luaL_newmetatable(L, regid);
    luaL_register(L, NULL, methods);
    /* metatable["__index"] = metatable */
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

static ptrdiff_t posrelat(ptrdiff_t pos, size_t len)
{
    /* relative string position: negative means back from end */
    if (pos < 0) pos += (ptrdiff_t)len + 1;
    return (pos >= 0) ? pos : 0;
}

static const char* v_checksubstring(lua_State *L, int narg, size_t* lp)
{
    size_t l;
    const char *s = luaL_checklstring(L, narg, &l);
    ptrdiff_t start = posrelat(luaL_optinteger(L, narg+1, 1), l);
    ptrdiff_t end = posrelat(luaL_optinteger(L, narg+2, -1), l);
    if (start < 1) start = 1;
    if (end > (ptrdiff_t)l) end = (ptrdiff_t)l;
    if (start <= end) {
        *lp = end-start+1;
        return s+start-1;
    } else {
        *lp = 0;
        return "";
    }
}

#define L_CRC_REGID "wt.bcrc"

static Crc* checkudata(lua_State* L)
{
    Crc** ud = (Crc**) luaL_checkudata(L, 1, L_CRC_REGID);

    luaL_argcheck(L, *ud, 1, "bcrc state has been destroyed");

    return *ud;
}

static Crc** newudata(lua_State* L)
{
    Crc** ud = (Crc**) lua_newuserdata(L, sizeof(*ud));
    *ud = NULL;

    luaL_getmetatable(L, L_CRC_REGID);
    lua_setmetatable(L, -2);

    return ud;
}

/*-
- crc = bcrc.new(bits, poly[, initial, xor, reflect_input, reflect_remainder])

Mandatory args:

  - bits=n, where n is 8, 16, 24, 32
  - poly=n, where n is the polynomial

Optional args:

  - initial=n, where n is the initial value for the crc, defaults to 0
  - xor=n, where n is the value to xor with the final value, defaults to 0
  - reflect_input=bool, defaults to false
  - reflect_remainder=bool, defaults to false

Returns a crc object.
*/
static int bcrc_new(lua_State *L)
{
    int bits = luaL_checkint(L, 1);
    int poly = luaL_checkint(L, 2);
    int initial = luaL_optint(L, 3, 0);
    int xor_ = luaL_optint(L, 4, 0);
    int reflect_input = lua_toboolean(L, 5);
    int reflect_remainder = lua_toboolean(L, 6);

    Crc** ud = newudata(L);

    switch(bits) {
        case  8: *ud = new CrcBasic< 8>(poly, initial, xor_, reflect_input, reflect_remainder); break;
        case 16: *ud = new CrcBasic<16>(poly, initial, xor_, reflect_input, reflect_remainder); break;
        case 24: *ud = new CrcBasic<24>(poly, initial, xor_, reflect_input, reflect_remainder); break;
        case 32: *ud = new CrcBasic<32>(poly, initial, xor_, reflect_input, reflect_remainder); break;
        default: return luaL_argerror(L, 2, "unsupported crc bit width");
    }

    luaL_argcheck(L, *ud, 1, "out of memory");

    return 1;
}

/*-
- crc = bcrc.crc16()

An optimal implementation of bcrc.new(16, 0x8005, 0, 0, true, true).
*/

/*-
- crc = bcrc.ccitt()

An optimal implementation of bcrc.new(16, 0x1021, 0xFFFF, 0, false, false).
*/

/*-
- crc = bcrc.xmodem()

An optimal implementation of bcrc.new(16, 0x8408, 0, 0, true, true).
*/

/*-
- crc = bcrc.crc32()

An optimal implementation of bcrc.new(32, 0x04C11DB7, 0xFFFFFFFF, 0xFFFFFFFF, true, true).
*/

/*
Above methods are all instantiated from a single template function.
*/
template < class Optimal >
static int bcrc_optimal(lua_State* L)
{
    Crc** ud = newudata(L);
    *ud = new CrcOptimal<Optimal>();
    luaL_argcheck(L, *ud, 1, "out of memory");
    return 1;
}

/*-
- self = crc:reset()

Resets the crc to it's initial state.

Returns the crc object.
*/
static int bcrc_reset (lua_State *L)
{
    Crc* ud = checkudata(L);
    ud->reset();
    return 1;
}

/*-
- self = crc:process(bytes, [start, [, end]])

Process substring of bytes from start..end.

If end is absent, it defaults to -1, the end of the bytes.
If start is absent, it defaults to 1, the start of the bytes.

Returns the crc object.
*/
static int bcrc_process(lua_State *L)
{
    Crc* ud = checkudata(L);
    size_t size = 0;
    const void* bytes = v_checksubstring(L, 2, &size);

    ud->process_bytes(bytes, size);

    lua_settop(L, 1);

    return 1;
}

#if 0
    /*-
    - self = crc:process_bit(bool)

    Process a single bit, a boolean.

    Returns the crc object.

    TODO - if necessary (but it can't be supported by optimal implementations)
    */
    static int bcrc_process_bit(lua_State *L)
    {
        Crc* ud = checkudata(L);

        luaL_checktype(L, 2, LUA_TBOOLEAN);

        ud->process_bit(lua_toboolean(L, 2));

        lua_settop(L, 1);

        return 1;
    }
#endif

/*-
- checksum = crc:checksum()

Returns the current crc checksum (it is possible to keep calling process()
after this).
*/
static int bcrc_checksum (lua_State *L)
{
    Crc* ud = checkudata(L);
    lua_pushinteger(L, ud->checksum());
    return 1;
}

/*-
- checksum = crc(bytes, ...)

Checksum in a single call, so

  checksum = crc(bytes, start, end)

is a short form for

  checksum = crc:reset():process(bytes, start, end):checksum()

See crc:process() for a description of bytes, start, end, and their default values.
*/
static int bcrc_call (lua_State *L)
{
    bcrc_reset(L);
    bcrc_process(L);
    return bcrc_checksum(L);
}

static int bcrc_gc (lua_State *L)
{
    Crc** ud = (Crc**) luaL_checkudata(L, 1, L_CRC_REGID);
    delete *ud;
    *ud = NULL;
    return 0;
}

static const luaL_reg bcrc_methods[] =
{
    {"reset",        bcrc_reset},
    {"process",      bcrc_process},
    {"checksum",     bcrc_checksum},
    {"__call",       bcrc_call},
    {"__gc",         bcrc_gc},
    {NULL, NULL}
};

static const luaL_reg bcrc[] =
{
    {"new",          bcrc_new},
    {"crc16",        bcrc_optimal<boost::crc_16_type>},
    {"ccitt",        bcrc_optimal<boost::crc_ccitt_type>},
    {"xmodem",       bcrc_optimal<boost::crc_xmodem_type>},
    {"crc32",        bcrc_optimal<boost::crc_32_type>},
    {NULL, NULL}
};

LUALIB_API int luaopen_bcrc (lua_State *L)
{
    v_obj_metatable(L, L_CRC_REGID, bcrc_methods);

    luaL_register(L, "bcrc", bcrc);

    return 1;
}

