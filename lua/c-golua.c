#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdint.h>
#include <stdio.h>
#include "_cgo_export.h"

#define MT_GOFUNCTION "GoLua.GoFunction"
#define MT_GOINTERFACE "GoLua.GoInterface"

#define GOLUA_DEFAULT_MSGHANDLER "golua_default_msghandler"

static const char GoStateRegistryKey = 'k'; //golua registry key
static const char PanicFIDRegistryKey = 'k';

int callback_function(lua_State* L);
int interface_index_callback(lua_State *L);
int interface_newindex_callback(lua_State *L);
int gchook_wrapper(lua_State* L);

typedef struct _chunk {
	int size; // chunk size
	char *buffer; // chunk data
	char* toread; // chunk to read
} chunk;

unsigned int *testudata(lua_State *L, int n, const char * field, void * expected)
{
	unsigned int *p = lua_touserdata(L, n);
	if (p != NULL)
	{  /* value is a userdata? */
		if (lua_getmetatable(L, n))
		{  /* does it have a metatable? */
			lua_getfield(L, -1, field);
			if (lua_tocfunction(L, -1) != expected)
				p = NULL;  /* wrong metatable */
			lua_pop(L, 2);  /* remove metatable and field values */
			return p;
		}
	}
	return NULL;  /* value is not a userdata with a metatable */
}

unsigned int *testgofunction(lua_State *L, int n)
{
	/* GoFunction should have __call callback */
	return testudata(L, n, "__call", callback_function);
}

unsigned int *testgostruct(lua_State *L, int n)
{
	/* GoInterface should have interface_index_callback */
	return testudata(L, n, "__index", interface_index_callback);
}

unsigned int *testgosomething(lua_State *L, int n)
{
	/* GoFunction and GoInterface should have gchook_wrapper callback */
	return testudata(L, n, "__gc", gchook_wrapper);
}

int clua_isgofunction(lua_State *L, int n)
{
	return testgofunction(L, n) != NULL;
}

int clua_isgostruct(lua_State *L, int n)
{
	return testgostruct(L, n) != NULL;
}

size_t clua_getgostate(lua_State* L)
{
	size_t gostateindex;
	//get gostate from registry entry
	lua_pushlightuserdata(L,(void*)&GoStateRegistryKey);
	lua_gettable(L, LUA_REGISTRYINDEX);
	gostateindex = (size_t)lua_touserdata(L,-1);
	lua_pop(L,1);
	return gostateindex;
}


//wrapper for callgofunction
int callback_function(lua_State* L)
{
	int r;
	unsigned int *fid = testgofunction(L, 1);
	size_t gostateindex = clua_getgostate(L);
	//remove the go function from the stack (to present same behavior as lua_CFunctions)
	lua_remove(L,1);
	return golua_callgofunction(gostateindex, fid!=NULL ? *fid : -1);
}

//wrapper for gchook
int gchook_wrapper(lua_State* L)
{
	//printf("Garbage collection wrapper\n");
	unsigned int* fid = testgosomething(L, -1);
	size_t gostateindex = clua_getgostate(L);
	if (fid != NULL)
		return golua_gchook(gostateindex,*fid);
	return 0;
}

unsigned int clua_togofunction(lua_State* L, int index)
{
	unsigned int *r = testgofunction(L, index);
	return (r != NULL) ? *r : -1;
}

unsigned int clua_togostruct(lua_State *L, int index)
{
	unsigned int *r = testgostruct(L, index);
	return (r != NULL) ? *r : -1;
}

void clua_pushgofunction(lua_State* L, unsigned int fid)
{
	unsigned int* fidptr = (unsigned int *)lua_newuserdata(L, sizeof(unsigned int));
	*fidptr = fid;
	luaL_getmetatable(L, MT_GOFUNCTION);
	lua_setmetatable(L, -2);
}

static int callback_c (lua_State* L)
{
	int fid = clua_togofunction(L,lua_upvalueindex(1));
	size_t gostateindex = clua_getgostate(L);
	return golua_callgofunction(gostateindex,fid);
}

void clua_pushcallback(lua_State* L)
{
	lua_pushcclosure(L,callback_c,1);
}

void clua_pushgostruct(lua_State* L, unsigned int iid)
{
	unsigned int* iidptr = (unsigned int *)lua_newuserdata(L, sizeof(unsigned int));
	*iidptr = iid;
	luaL_getmetatable(L, MT_GOINTERFACE);
	lua_setmetatable(L,-2);
}

int default_panicf(lua_State *L)
{
	const char *s = lua_tostring(L, -1);
	printf("Lua unprotected panic: %s\n", s);
	abort();
}

void clua_setgostate(lua_State* L, size_t gostateindex)
{
	lua_atpanic(L, default_panicf);
	lua_pushlightuserdata(L,(void*)&GoStateRegistryKey);
	lua_pushlightuserdata(L, (void*)gostateindex);
	//set into registry table
	lua_settable(L, LUA_REGISTRYINDEX);
}

static int writer (lua_State *L, const void* b, size_t size, void* B) {
	static int count=0;
	(void)L;
	luaL_addlstring((luaL_Buffer*) B, (const char *)b, size);
	return 0;
}

// dump function chunk from luaL_loadstring
int dump_chunk (lua_State *L) {
	luaL_Buffer b;
	luaL_checktype(L, -1, LUA_TFUNCTION);
	lua_settop(L, -1);
	luaL_buffinit(L,&b);
	int errno;
	errno = lua_dump(L, writer, &b);
	if (errno != 0){
	return luaL_error(L, "unable to dump given function, errno:%d", errno);
	}
	luaL_pushresult(&b);
	return 0;
}

static const char * reader (lua_State *L, void *ud, size_t *sz) {
	chunk *ck = (chunk *)ud;
	if (ck->size > LUAL_BUFFERSIZE) {
		ck->size -= LUAL_BUFFERSIZE;
		*sz = LUAL_BUFFERSIZE;
		ck->toread = ck->buffer;
		ck->buffer += LUAL_BUFFERSIZE;
	}else{
		*sz = ck->size;
		ck->toread = ck->buffer;
		ck->size = 0;
	}
	return ck->toread;
}

// load function chunk dumped from dump_chunk
int load_chunk(lua_State *L, char *b, int size, const char* chunk_name) {
	chunk ck;
	ck.buffer = b;
	ck.size = size;
	int errno;
	errno = lua_load(L, reader, &ck, chunk_name);
	if (errno != 0) {
		return luaL_error(L, "unable to load chunk, errno: %d", errno);
	}
	return 0;
}

/* called when lua code attempts to access a field of a published go object */
int interface_index_callback(lua_State *L)
{
	unsigned int *iid = testgostruct(L, 1);
	if (iid == NULL)
	{
		lua_pushnil(L);
		return 1;
	}

	char *field_name = (char *)lua_tostring(L, 2);
	if (field_name == NULL)
	{
		lua_pushnil(L);
		return 1;
	}

	size_t gostateindex = clua_getgostate(L);

	int r = golua_interface_index_callback(gostateindex, *iid, field_name);

	if (r < 0)
	{
		lua_error(L);
		return 0;
	}
	else if (r == 0)  // no field
	{
		// If a custom metatable was set for this object with clua_gostructmetatable(),
		// the original __index value should be available as upvalue[1].
		switch (lua_type(L, lua_upvalueindex(1))) {
		case LUA_TNIL:  /* no user metatable with __index was set */
			luaL_error(L, "No field: %s", field_name);
			return 0;
		case LUA_TFUNCTION:
		FUNCTION:
			lua_pushvalue(L, lua_upvalueindex(1));  // put __index onto the stack
			lua_insert(L, 1);  // move the function value to the beginning of the stack
			lua_call(L, 2, 1);  // Call __index(obj, field_name)
			return 1;
		case LUA_TUSERDATA:
			if (clua_isgofunction(L, lua_upvalueindex(1))) {
				goto FUNCTION;
			}
			/* fallthrough */
		default:
			lua_pushvalue(L, lua_upvalueindex(1));  // put __index onto the stack
			lua_getfield(L, -1, field_name);  // __index[field_name]
			return 1;
		}
	}
	else
	{
		return r;
	}
}

/* called when lua code attempts to set a field of a published go object */
int interface_newindex_callback(lua_State *L)
{
	unsigned int *iid = testgostruct(L, 1);
	if (iid == NULL)
	{
		lua_pushnil(L);
		return 1;
	}

	char *field_name = (char *)lua_tostring(L, 2);
	if (field_name == NULL)
	{
		lua_pushnil(L);
		return 1;
	}

	size_t gostateindex = clua_getgostate(L);

	int r = golua_interface_newindex_callback(gostateindex, *iid, field_name);

	if (r < 0)
	{
		lua_error(L);
		return 0;
	}
	else if (r == 0)  // no field
	{
		// If a custom metatable was set for this object with clua_gostructmetatable(),
		// the original __newindex value should be available as upvalue[1].
		switch (lua_type(L, lua_upvalueindex(1))) {
		case LUA_TNIL:  /* no user metatable with __newindex was set */
			luaL_error(L, "Wrong assignment to field %s", field_name);
			return 0;
		case LUA_TFUNCTION:
		FUNCTION:
			lua_pushvalue(L, lua_upvalueindex(1));  // put __newindex function onto the stack
			lua_insert(L, 1);  // move the function value to the beginning of the stack
			lua_call(L, 3, 1);  // Call __newindex(obj, field_name, value)
			return 1;
		case LUA_TUSERDATA:
			if (clua_isgofunction(L, lua_upvalueindex(1))) {
				goto FUNCTION;
			}
			/* fallthrough */
		default:
			lua_pushvalue(L, lua_upvalueindex(1));  // put __newindex onto the stack
			lua_pushvalue(L, 3);  // push the value
			lua_setfield(L, -2, field_name);  // __newindex[field_name] = value, pops the value
			lua_pop(L, 1); // pop __newindex
			return 1;
		}
	}
	else
	{
		return r;
	}
}

int panic_msghandler(lua_State *L)
{
	size_t gostateindex = clua_getgostate(L);
	go_panic_msghandler(gostateindex, (char *)lua_tolstring(L, -1, NULL));
	return 0;
}

void clua_hide_pcall(lua_State *L)
{
	lua_getglobal(L, "pcall");
	lua_setglobal(L, "unsafe_pcall");
	lua_pushnil(L);
	lua_setglobal(L, "pcall");

	lua_getglobal(L, "xpcall");
	lua_setglobal(L, "unsafe_xpcall");
	lua_pushnil(L);
	lua_setglobal(L, "xpcall");
}

void clua_initstate(lua_State* L)
{
	/* create the GoLua.GoFunction metatable */
	luaL_newmetatable(L, MT_GOFUNCTION);

	// gofunction_metatable[__call] = &callback_function
	lua_pushliteral(L,"__call");
	lua_pushcfunction(L,&callback_function);
	lua_settable(L,-3);

	// gofunction_metatable[__gc] = &gchook_wrapper
	lua_pushliteral(L,"__gc");
	lua_pushcfunction(L,&gchook_wrapper);
	lua_settable(L,-3);
	lua_pop(L,1);

	luaL_newmetatable(L, MT_GOINTERFACE);

	// gointerface_metatable[__gc] = &gchook_wrapper
	lua_pushliteral(L, "__gc");
	lua_pushcfunction(L, &gchook_wrapper);
	lua_settable(L, -3);

	// gointerface_metatable[__index] = &interface_index_callback
	lua_pushliteral(L, "__index");
	lua_pushcfunction(L, &interface_index_callback);
	lua_settable(L, -3);

	// gointerface_metatable[__newindex] = &interface_newindex_callback
	lua_pushliteral(L, "__newindex");
	lua_pushcfunction(L, &interface_newindex_callback);
	lua_settable(L, -3);

	lua_register(L, GOLUA_DEFAULT_MSGHANDLER, &panic_msghandler);
	lua_pop(L, 1);
}


int callback_panicf(lua_State* L)
{
	lua_pushlightuserdata(L,(void*)&PanicFIDRegistryKey);
	lua_gettable(L,LUA_REGISTRYINDEX);
	unsigned int fid = lua_tointeger(L,-1);
	lua_pop(L,1);
	size_t gostateindex = clua_getgostate(L);
	return golua_callpanicfunction(gostateindex,fid);

}

//TODO: currently setting garbage when panicf set to null
GoInterface clua_atpanic(lua_State* L, unsigned int panicf_id)
{
	//get old panicfid
	unsigned int old_id;
	lua_pushlightuserdata(L, (void*)&PanicFIDRegistryKey);
	lua_gettable(L,LUA_REGISTRYINDEX);
	if(lua_isnil(L, -1) == 0)
		old_id = lua_tointeger(L,-1);
	lua_pop(L, 1);

	//set registry key for function id of go panic function
	lua_pushlightuserdata(L, (void*)&PanicFIDRegistryKey);
	//push id value
	lua_pushinteger(L, panicf_id);
	//set into registry table
	lua_settable(L, LUA_REGISTRYINDEX);

	//now set the panic function
	lua_CFunction pf = lua_atpanic(L,&callback_panicf);
	//make a GoInterface with a wrapped C panicf or the original go panicf
	if(pf == &callback_panicf)
	{
		return golua_idtointerface(old_id);
	}
	else
	{
		//TODO: technically UB, function ptr -> non function ptr
		return golua_cfunctiontointerface((GoUintptr *)pf);
	}
}

int clua_callluacfunc(lua_State* L, lua_CFunction f)
{
	return f(L);
}

void* allocwrapper(void* ud, void *ptr, size_t osize, size_t nsize)
{
	return (void*)golua_callallocf((GoUintptr)ud,(GoUintptr)ptr,osize,nsize);
}

lua_State* clua_newstate(void* goallocf)
{
	return lua_newstate(&allocwrapper,goallocf);
}

void clua_setallocf(lua_State* L, void* goallocf)
{
	lua_setallocf(L,&allocwrapper,goallocf);
}

void clua_openbase(lua_State* L)
{
	lua_pushcfunction(L,&luaopen_base);
	lua_pushstring(L,"");
	lua_call(L, 1, 0);
	clua_hide_pcall(L);
}

void clua_openio(lua_State* L)
{
	lua_pushcfunction(L,&luaopen_io);
	lua_pushstring(L,"io");
	lua_call(L, 1, 0);
}

void clua_openmath(lua_State* L)
{
	lua_pushcfunction(L,&luaopen_math);
	lua_pushstring(L,"math");
	lua_call(L, 1, 0);
}

void clua_openpackage(lua_State* L)
{
	lua_pushcfunction(L,&luaopen_package);
	lua_pushstring(L,"package");
	lua_call(L, 1, 0);
}

void clua_openstring(lua_State* L)
{
	lua_pushcfunction(L,&luaopen_string);
	lua_pushstring(L,"string");
	lua_call(L, 1, 0);
}

void clua_opentable(lua_State* L)
{
	lua_pushcfunction(L,&luaopen_table);
	lua_pushstring(L,"table");
	lua_call(L, 1, 0);
}

void clua_openos(lua_State* L)
{
	lua_pushcfunction(L,&luaopen_os);
	lua_pushstring(L,"os");
	lua_call(L, 1, 0);
}

void clua_hook_function(lua_State *L, lua_Debug *ar)
{
	lua_checkstack(L, 2);
	lua_pushstring(L, "Lua execution quantum exceeded");
	lua_error(L);
}

void clua_setexecutionlimit(lua_State* L, int n)
{
	lua_sethook(L, &clua_hook_function, LUA_MASKCOUNT, n);
}

// Modifies the table at the top of the stack to use it as a metatable for GoStruct object
void clua_gostructmetatable(lua_State* L)
{
	// gointerface_metatable[__gc] = &gchook_wrapper
	lua_pushliteral(L, "__gc");
	lua_pushcfunction(L, &gchook_wrapper);
	lua_settable(L, -3);

	// gointerface_metatable[__index] = &interface_index_callback
	lua_pushliteral(L, "__index");
	// if replacing an __index field, store the original one as an upvalue
	lua_getfield(L, -2, "__index");
	if (!lua_isnil(L, -1)) {
		// store the original __index as an upvalue
		lua_pushcclosure(L, &interface_index_callback, 1);
	} else {
		lua_pop(L, 1); // pop nil
		lua_pushcfunction(L, &interface_index_callback);
	}
	lua_settable(L, -3);

	// gointerface_metatable[__newindex] = &interface_newindex_callback
	lua_pushliteral(L, "__newindex");
	// if replacing a __newindex field, store the original one as an upvalue
	lua_getfield(L, -2, "__newindex");
	if (!lua_isnil(L, -1)) {
		// store the original __newindex as an upvalue
		lua_pushcclosure(L, &interface_newindex_callback, 1);
	} else {
		lua_pop(L, 1); // pop nil
		lua_pushcfunction(L, &interface_newindex_callback);
	}
	lua_settable(L, -3);
}
