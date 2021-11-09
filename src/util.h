#define internal static

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef i32 b32;

#define false 0
#define true 1
#define I8_MIN  -128
#define I8_MAX  127
#define U8_MAX  0xff
#define U32_MAX 0xffffffff

#define array_count(x) (sizeof(x) / sizeof(x[0]))
#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

typedef struct
{
  i32 x;
  i32 y;
} v2i;

internal b32 v2i_eq(v2i a, v2i b)
{
  return a.x == b.x && a.y == b.y;
}

typedef union
{
  struct
  {
    u8 x, y, z;
  };

  struct
  {
    u8 r, g, b;
  };
} v3u8;

internal b32 v3u8_eq(v3u8 a, v3u8 b)
{
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

typedef struct
{
  size_t size;
  u8* data;
} str_t;
#define str(x) ((str_t){ sizeof(x) - 1, (u8*)(x) })

internal str_t wrap_str(char* z)
{
  str_t result = {0};
  result.data = (u8*)z;
  while(*z) { ++z; }
  result.size = (u8*)z - result.data;
  return result;
}

internal b32 zstr_eq(char* a, char* b)
{
  b32 result = true;
  while(*a && *b && result)
  {
    result &= (*a++ == *b++);
  }
  result &= (*a++ == *b++);
  return result;
}

internal b32 str_eq(str_t a, str_t b)
{
  b32 result = true;
  if(a.size == b.size)
  {
    for(u32 i = 0;
        i < a.size && result;
        ++i)
    {
      if(a.data[i] != b.data[i])
      {
        result = false;
      }
    }
  }
  else
  {
    result = false;
  }
  return result;
}

internal str_t read_file(char* path)
{
  str_t result = {0};
  FILE* fd = fopen(path, "rb");
  if(fd == 0)
  {
    fprintf(stderr, "Could not open file '%s' for reading\n", path);
  }
  else
  {
    if(fseek(fd, 0, SEEK_END) == -1)
    {
      fprintf(stderr, "Could not seek to end of file '%s'\n", path);
    }
    else
    {
      result.size = ftell(fd);
      fseek(fd, 0L, SEEK_SET);
      result.data = malloc(result.size);

      if(!result.data)
      {
        fprintf(stderr, "Could not allocate %lu bytes for file '%s'\n", result.size, path);
      }
      else if(fread(result.data, 1, result.size, fd) != result.size)
      {
        fprintf(stderr, "Could not read all of file '%s'\n", path);
      }
    }
  }

  if(fd)
  {
    fclose(fd);
  }

  return result;
}

typedef struct arena_block_t
{
  size_t capacity;
  size_t used;
  void* data;

  struct arena_block_t* previous_block;
} arena_block_t;

typedef struct
{
  arena_block_t* head;
} arena_t;

typedef struct
{
  arena_t* arena;
  arena_block_t* block;
  size_t used;
} arena_snap_t;

internal arena_t new_arena()
{
  arena_t result = {};
  return result;
}

internal arena_snap_t arena_snap(arena_t* arena)
{
  arena_block_t* block = arena->head;
  arena_snap_t result = {
    .arena = arena,
    .block = block,
  };
  if(block)
  {
    result.used = block->used;
  }
  return result;
}

internal void arena_restore(arena_snap_t snap)
{
  arena_t* arena = snap.arena;

  while(arena->head != snap.block)
  {
    arena_block_t* block = arena->head;
    assert(block);
    arena->head = block->previous_block;
    free(block);
  }

  if(arena->head)
  {
    arena->head->used = snap.used;
  }
}

internal void clear_arena(arena_t* arena)
{
  while(arena->head)
  {
    arena_block_t* block = arena->head;
    arena->head = block->previous_block;
    free(block);
  }
}

internal void* alloc_bytes(arena_t* arena, size_t size)
{
  void* result = 0;

  b32 allocate = !arena->head || (arena->head->used + size > arena->head->capacity);
  if(allocate)
  {
    size_t default_capacity = 8 * 1024 * 1024;
    size_t capacity = (size > default_capacity) ? size : default_capacity;
    // printf("* Allocating %lu bytes capacity\n", capacity);
    arena_block_t* block = malloc(sizeof(arena_block_t) + capacity);
    if(block)
    {
      *block = (arena_block_t){
        .capacity = capacity,
        .data = (void*)(block + 1),
        .previous_block = arena->head,
      };
      arena->head = block;
    }
  }

  if(arena->head)
  {
    result = arena->head->data + arena->head->used;
    arena->head->used += size;
  }

  return result;
}
#define alloc_struct(arena, type) ((type*)alloc_bytes((arena), sizeof(type)))
#define alloc_array(arena, count, type) ((type*)alloc_bytes((arena), (count) * sizeof(type)))

internal void* alloc_bytes_clear(arena_t* arena, size_t size)
{
  void* result = alloc_bytes(arena, size);
  if(result)
  {
    u8* bytes = result;
    for(size_t i = 0;
        i < size;
        ++i)
    {
      *bytes++ = 0;
    }
  }
  return result;
}
#define alloc_struct_clear(arena, type) ((type*)alloc_bytes_clear((arena), sizeof(type)))
#define alloc_array_clear(arena, count, type) ((type*)alloc_bytes_clear((arena), (count) * sizeof(type)))

internal b32 is_ascii(u8 c)
{
  return (c & 0x80) == 0;
}

internal b32 is_printable(u8 c)
{
  return c >= ' ' && c <= '~';
}

internal b32 is_upper(u8 c)
{
  return c >= 'A' && c <= 'Z';
}

internal b32 is_lower(u8 c)
{
  return c >= 'a' && c <= 'z';
}

internal b32 is_linebreak(u8 c)
{
  return c >= '\n' && c <= '\r';
}

internal b32 is_alpha(u8 c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

internal u8 to_lower(u8 c)
{
  u8 result = c;

  if(is_alpha(c))
  {
    result |= 0x20;
  }

  return result;
}

internal u8 to_upper(u8 c)
{
  u8 result = c;

  if(is_alpha(c))
  {
    result &= ~0x20;
  }

  return result;
}
