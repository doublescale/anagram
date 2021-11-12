#undef _FORTIFY_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <pthread.h>

#include "util.h"

typedef struct
{
  i8 counts[27];
} breakdown_t;

typedef struct wordlink_t
{
  str_t word;
  struct wordlink_t* next;
} wordlink_t;

typedef struct keylink_t
{
  breakdown_t key;
  wordlink_t first_word;

  struct keylink_t* next;
} keylink_t;

typedef struct
{
  keylink_t* entries[128 * 1024];
} hashtable_t;

internal breakdown_t breakdown_word(str_t word)
{
  breakdown_t result = {0};

  for(u32 idx = 0;
      idx < word.size;
      ++idx)
  {
    u8 c = word.data[idx];
    if(is_alpha(c))
    {
      u32 breakdown_idx = to_lower(c) - 'a';
      assert(breakdown_idx < array_count(result.counts));
      assert(result.counts[breakdown_idx] < I8_MAX);
      ++result.counts[breakdown_idx];
    }
  }

  return result;
}

internal b32 breakdown_eq(breakdown_t* a, breakdown_t* b)
{
  b32 equal = true;

  for(u32 idx = 0;
      idx < array_count(a->counts) && equal;
      ++idx)
  {
    equal = (a->counts[idx] == b->counts[idx]);
  }

  return equal;
}

internal b32 breakdown_is_empty(breakdown_t* a)
{
  b32 all_zero = true;

  for(u32 idx = 0;
      idx < array_count(a->counts) && all_zero;
      ++idx)
  {
    all_zero = (a->counts[idx] == 0);
  }

  return all_zero;
}

internal b32 breakdown_is_positive(breakdown_t* a)
{
  b32 underflowed = false;
  b32 positive = false;

  for(u32 idx = 0;
      idx < array_count(a->counts) && !underflowed;
      ++idx)
  {
    underflowed = (a->counts[idx] < 0);
    positive |= (a->counts[idx] > 0);
  }

  return !underflowed && positive;
}

internal b32 breakdown_underflowed(breakdown_t* a)
{
  b32 underflowed = false;

  for(u32 idx = 0;
      idx < array_count(a->counts) && !underflowed;
      ++idx)
  {
    underflowed = (a->counts[idx] < 0);
  }

  return underflowed;
}

internal b32 breakdown_contains(breakdown_t* a, breakdown_t* b)
{
  b32 contains = true;

  for(u32 idx = 0;
      idx < array_count(a->counts) && contains;
      ++idx)
  {
    contains = (a->counts[idx] >= b->counts[idx]);
  }

  return contains;
}

internal void breakdown_add(breakdown_t* a, breakdown_t* b)
{
  for(u32 idx = 0;
      idx < array_count(a->counts);
      ++idx)
  {
    i32 sum = (i32)a->counts[idx] + (i32)b->counts[idx];
    assert(sum <= I8_MAX);
    a->counts[idx] = (i8)sum;
  }
}

internal b32 breakdown_subtract(breakdown_t* a, breakdown_t* b)
{
  b32 negative = false;

  for(u32 idx = 0;
      idx < array_count(a->counts);
      ++idx)
  {
    i32 difference = (i32)a->counts[idx] - (i32)b->counts[idx];
    assert(difference >= I8_MIN);
    a->counts[idx] = (i8)difference;

    negative = negative || (difference < 0);
  }

  return !negative;
}

internal i32 breakdown_sum(breakdown_t* a)
{
  i32 sum = 0;

  for(u32 idx = 0;
      idx < array_count(a->counts);
      ++idx)
  {
    sum += a->counts[idx];
  }

  return sum;
}

internal void breakdown_max0(breakdown_t* a)
{
  for(u32 idx = 0;
      idx < array_count(a->counts);
      ++idx)
  {
    a->counts[idx] = max(0, a->counts[idx]);;
  }
}

internal void print_breakdown(breakdown_t* breakdown)
{
  for(u32 idx = 0;
      idx < array_count(breakdown->counts);
      ++idx)
  {
    i8 count = breakdown->counts[idx];
    for(u32 j = 0;
        j < count;
        ++j)
    {
      printf("%c", 'a' + idx);
    }
  }
}

internal u32 hash_breakdown(breakdown_t* breakdown)
{
  u32 result = 1;

  for(u32 idx = 0;
      idx < array_count(breakdown->counts);
      ++idx)
  {
    i8 count = breakdown->counts[idx];
    result = 107 * result + count;
  }

  return result;
}

internal void hashtable_add_word(
    hashtable_t* hashtable, arena_t* arena, str_t word, breakdown_t* breakdown)
{
  u32 hash = hash_breakdown(breakdown);
  u32 entry_idx = hash % array_count(hashtable->entries);
  keylink_t** hashtable_entry = hashtable->entries + entry_idx;

  keylink_t** same_key_entry = hashtable_entry;
  while(*same_key_entry && !breakdown_eq(&(*same_key_entry)->key, breakdown))
  {
    same_key_entry = &(*same_key_entry)->next;
  }

  if(!*same_key_entry)
  {
    keylink_t* link = alloc_struct_clear(arena, keylink_t);
    link->key = *breakdown;
    link->first_word.word = word;
    link->first_word.next = 0;
    link->next = *hashtable_entry;
    *hashtable_entry = link;
  }
  else
  {
    // TODO: Only insert word if it hasn't appeared yet.
    //       Iterate word links, abort if seen, append to end?
    keylink_t* link = *same_key_entry;
    wordlink_t* new_word = alloc_struct(arena, wordlink_t);
    *new_word = link->first_word;
    link->first_word.word = word;
    link->first_word.next = new_word;
  }
}

internal void list_anagram_groups(hashtable_t* hashtable, arena_t* arena, u32 min_word_count)
{
  typedef struct anagram_group_t
  {
    u32 word_count;
    keylink_t* keylink;
    struct anagram_group_t* next;
  } anagram_group_t;

  anagram_group_t* groups = 0;
  for(u32 entry_idx = 0;
      entry_idx < array_count(hashtable->entries);
      ++entry_idx)
  {
    for(keylink_t* keylink = hashtable->entries[entry_idx];
        keylink;
        keylink = keylink->next)
    {
      u32 word_count = 0;
      for(wordlink_t* word_link = &keylink->first_word;
          word_link;
          word_link = word_link->next)
      {
        ++word_count;
      }

      if(word_count >= min_word_count)
      {
        // Insert biggest groups first.
        anagram_group_t** next_smallest_group = &groups;
        while(*next_smallest_group && (*next_smallest_group)->word_count >= word_count)
        {
          next_smallest_group = &(*next_smallest_group)->next;
        }

        anagram_group_t* new_group = alloc_struct(arena, anagram_group_t);
        new_group->word_count = word_count;
        new_group->keylink = keylink;
        new_group->next = *next_smallest_group;
        *next_smallest_group = new_group;
      }
    }
  }

  for(anagram_group_t* group = groups;
      group;
      group = group->next)
  {
    printf("\n");
    for(wordlink_t* word_link = &group->keylink->first_word;
        word_link;
        word_link = word_link->next)
    {
      str_t word = word_link->word;
      printf("%.*s\n", (int)word.size, word.data);
    }
  }
}

internal void list_anagrams_for(hashtable_t* hashtable, arena_t* arena,
    breakdown_t input_breakdown, str_t must_include, str_t space_separated_must_exclude,
    i32 max_results)
{
  breakdown_t reduced_input_breakdown = input_breakdown;
  breakdown_t must_include_breakdown = breakdown_word(must_include);
  b32 must_include_is_valid = breakdown_subtract(&reduced_input_breakdown, &must_include_breakdown);

  if(!must_include_is_valid)
  {
    breakdown_t missing_letters = must_include_breakdown;
    breakdown_subtract(&missing_letters, &input_breakdown);
    breakdown_max0(&missing_letters);
    printf("Missing %d letters:\n", breakdown_sum(&missing_letters));
    for(u32 breakdown_idx = 0;
        breakdown_idx < array_count(missing_letters.counts);
        ++breakdown_idx)
    {
      i8 count = missing_letters.counts[breakdown_idx];
      if(count != 0)
      {
        printf("  %dx '%c'\n", (int)count, 'a' + breakdown_idx);
      }
    }

    printf("\nPossible additions:\n");
    list_anagrams_for(hashtable, arena, missing_letters, str(""), str(""), 20);
  }
  else if(breakdown_is_empty(&reduced_input_breakdown))
  {
    printf("  %.*s\n", (int)must_include.size, must_include.data);
  }
  else
  {
    wordlink_t* excluded_words = 0;

    // Separate excluded words by space.
    for(i32 idx = 0, last_wordstart = 0;
        idx <= space_separated_must_exclude.size;
        ++idx)
    {
      if(idx == space_separated_must_exclude.size ||
          space_separated_must_exclude.data[idx] == ' ')
      {
        i32 size = idx - last_wordstart;
        if(size > 0)
        {
          str_t word = {
            .size = (u32)size,
            .data = space_separated_must_exclude.data + last_wordstart,
          };
          wordlink_t* new_excluded_word = alloc_struct(arena, wordlink_t);
          new_excluded_word->word = word;
          new_excluded_word->next = excluded_words;
          excluded_words = new_excluded_word;
        }
        last_wordstart = idx + 1;
      }
    }

    // Find words that could fit into the input.
    keylink_t* subkeys = 0;
    for(u32 entry_idx = 0;
        entry_idx < array_count(hashtable->entries);
        ++entry_idx)
    {
      for(keylink_t* key_link = hashtable->entries[entry_idx];
          key_link;
          key_link = key_link->next)
      {
        if(breakdown_contains(&reduced_input_breakdown, &key_link->key))
        {
          wordlink_t* current_wordlink = 0;

          for(wordlink_t* test_wordlink = &key_link->first_word;
              test_wordlink;
              test_wordlink = test_wordlink->next)
          {
            str_t word = test_wordlink->word;

            b32 excluded = false;
            for(wordlink_t* excluded_word = excluded_words;
                excluded_word && !excluded;
                excluded_word = excluded_word->next)
            {
              if(str_eq(word, excluded_word->word))
              {
                excluded = true;
              }
            }

            if(!excluded)
            {
              if(!current_wordlink)
              {
                // Insert longest words first.
                u32 key_sum = breakdown_sum(&key_link->key);
                keylink_t** subkey = &subkeys;
                while(*subkey && breakdown_sum(&(*subkey)->key) >= key_sum)
                {
                  subkey = &(*subkey)->next;
                }

                keylink_t* new_subkey = alloc_struct(arena, keylink_t);
                new_subkey->key = key_link->key;
                new_subkey->next = *subkey;
                *subkey = new_subkey;
                current_wordlink = &new_subkey->first_word;
              }
              else
              {
                wordlink_t* new_word = alloc_struct(arena, wordlink_t);
                current_wordlink->next = new_word;
                current_wordlink = new_word;
              }

              current_wordlink->word = word;
              current_wordlink->next = 0;
            }
          }
        }
      }
    }

#if 0
    printf("Subkeys:\n");
    for(keylink_t* subkey = subkeys;
        subkey;
        subkey = subkey->next)
    {
      printf(" ");
      u32 col = 1;
      for(wordlink_t* word_link = &subkey->first_word;
          word_link;
          word_link = word_link->next)
      {
        str_t word = word_link->word;

        col += 1 + word.size;
        if(col > 80)
        {
          printf("\n   ");
          col = 4 + word.size;
        }

        printf(" %.*s", (int)word.size, word.data);
      }
      printf("\n");
    }
    printf("\n");
#endif

    if(subkeys)
    {
      u32 chain_max_length = max(1, breakdown_sum(&input_breakdown));
      u32 chain_length = 0;
      keylink_t** chain = alloc_array(arena, chain_max_length, keylink_t*);

      breakdown_t remaining_breakdown = reduced_input_breakdown;
      chain[chain_length++] = subkeys;
      keylink_t* next_min_subkey = chain[chain_length - 1];
      b32 no_underflow = breakdown_subtract(&remaining_breakdown, &subkeys->key);
      assert(no_underflow);

      i32 result_count = 0;
      while(chain_length > 0 && (max_results < 0 || result_count < max_results))
      {
        if(breakdown_is_empty(&remaining_breakdown))
        {
          // Print results, with per-word anagram combinations.
          arena_snap_t snap = arena_snap(arena);

          wordlink_t** tmp_links = alloc_array(arena, chain_length, wordlink_t*);
          for(u32 link_idx = 0;
              link_idx < chain_length;
              ++link_idx)
          {
            tmp_links[link_idx] = &chain[link_idx]->first_word;
          }

          while(max_results < 0 || result_count < max_results)
          {
            printf("  ");
            if(must_include.size > 0)
            {
              printf("%.*s ", (int)must_include.size, must_include.data);
            }

            for(u32 link_idx = 0;
                link_idx < chain_length;
                ++link_idx)
            {
              str_t word = tmp_links[link_idx]->word;

              if(link_idx > 0) { printf(" "); }
              printf("%.*s", (int)word.size, word.data);
            }
            printf("\n");

            ++result_count;

            // Go to next per-word anagram permutation.
            tmp_links[0] = tmp_links[0]->next;
            for(u32 link_idx = 0;
                link_idx < chain_length - 1;
                ++link_idx)
            {
              if(!tmp_links[link_idx])
              {
                tmp_links[link_idx] = &chain[link_idx]->first_word;
                tmp_links[link_idx + 1] = tmp_links[link_idx + 1]->next;
              }
            }
            if(!tmp_links[chain_length - 1])
            {
              break;
            }
          }

          arena_restore(snap);
        }

        b32 found_next = false;
        // Try adding a new chain element.
        for(keylink_t* next_subkey = next_min_subkey;
            next_subkey && !found_next;
            next_subkey = next_subkey->next)
        {
          breakdown_t* next_key = &next_subkey->key;
          if(breakdown_contains(&remaining_breakdown, next_key))
          {
            assert(chain_length < chain_max_length);
            chain[chain_length++] = next_subkey;
            breakdown_subtract(&remaining_breakdown, next_key);
            found_next = true;
            next_min_subkey = chain[chain_length - 1];
          }
        }

        if(!found_next)
        {
          // Try changing the last chain element.
          keylink_t* prev_last_subkey = chain[--chain_length];
          breakdown_add(&remaining_breakdown, &prev_last_subkey->key);
          for(keylink_t* next_subkey = prev_last_subkey->next;
              next_subkey && !found_next;
              next_subkey = next_subkey->next)
          {
            breakdown_t* next_key = &next_subkey->key;
            if(breakdown_contains(&remaining_breakdown, next_key))
            {
              assert(chain_length < chain_max_length);
              chain[chain_length++] = next_subkey;
              breakdown_subtract(&remaining_breakdown, next_key);
              found_next = true;
              next_min_subkey = chain[chain_length - 1];
            }
          }

          if(!found_next)
          {
            next_min_subkey = prev_last_subkey->next;
          }
        }
      }
    }
  }
}

typedef struct
{
  u32 count;
  char** values;
} counted_args_t;

internal char* pop_arg(counted_args_t* args)
{
  char* result = 0;
  if(args->count)
  {
    result = args->values[0];
    ++args->values;
    --args->count;
  }
  return result;
}

#include "terminal_io.h"

internal void draw_char(char_frame_t* frame, v3u8 fg_col, v3u8 bg_col, i32 x, i32 y, u8 c)
{
  if(x >= 0 && y >= 0 && x < frame->width && y < frame->height)
  {
    frame->chars[frame->width * y + x] = (color_char_t){fg_col, bg_col, c};
  }
}

internal i32 draw_str(char_frame_t* frame, v3u8 fg_col, v3u8 bg_col, i32 start_x, i32 y, str_t str)
{
  if(y >= 0 && y < frame->height)
  {
    u32 start_i = (u32)max(0, -start_x);
    for(u32 i = start_i;
        i < str.size;
        ++i)
    {
      i32 x = start_x + i;
      if(x >= frame->width) { break; }
      draw_char(frame, fg_col, bg_col, x, y, str.data[i]);
    }
  }

  return str.size;
}

v3u8 black       = {0,0,0};
v3u8 dark_gray   = {80,80,80};
v3u8 bright_gray = {140,140,140};
v3u8 white       = {255,255,255};
v3u8 dark_red    = {160,0,0};
v3u8 bright_red  = {255,0,0};

typedef struct anagram_result_t
{
  u32 word_count;
  str_t* words;
  struct anagram_result_t* next_result;
} anagram_result_t;

typedef struct
{
  arena_t arena;

  u32 result_count;
  b32 not_done;
  anagram_result_t* first_result;
  anagram_result_t* last_result;
} anagram_results_t;

typedef struct
{
  b32 initialized;

  arena_t* tmp_arena;
  arena_snap_t tmp_arena_snap;
  keylink_t* subkeys; // not used at the moment; could visualize

  u32 chain_max_length;
  u32 chain_length;
  keylink_t** chain;

  breakdown_t remaining_breakdown;
  keylink_t* next_subkey_to_add;

  anagram_results_t results;
} anagram_context_t;

internal anagram_result_t* begin_anagram_result(anagram_results_t* results, u32 word_count)
{
  anagram_result_t* result = alloc_struct_clear(&results->arena, anagram_result_t);
  result->word_count = word_count;
  result->words = alloc_array_clear(&results->arena, word_count, str_t);

  if(results->last_result)
  {
    results->last_result->next_result = result;
  }
  else
  {
    results->first_result = result;
  }
  results->last_result = result;
  ++results->result_count;

  return result;
}

internal anagram_context_t begin_anagram_context(hashtable_t* hashtable, arena_t* arena,
    breakdown_t* input_breakdown,
    breakdown_t* must_include_breakdown,
    str_t space_separated_must_exclude)
{
  anagram_context_t ctx = {0};
  ctx.initialized = true;
  ctx.tmp_arena = arena;
  ctx.tmp_arena_snap = arena_snap(arena);
  ctx.results.arena = new_custom_arena(1024 * 1024);
  ctx.results.not_done = true;

  breakdown_t reduced_input_breakdown = *input_breakdown;
  b32 must_include_is_valid = breakdown_subtract(&reduced_input_breakdown, must_include_breakdown);

  if(!must_include_is_valid)
  {
    // TODO: Suggest possible words to add, like in list_anagrams_for.
  }
  else if(!breakdown_is_empty(must_include_breakdown)
      && breakdown_is_empty(&reduced_input_breakdown))
  {
    begin_anagram_result(&ctx.results, 0);
  }
  else
  {
    wordlink_t* excluded_words = 0;

    // Separate excluded words by space.
    for(i32 idx = 0, last_wordstart = 0;
        idx <= space_separated_must_exclude.size;
        ++idx)
    {
      if(idx == space_separated_must_exclude.size ||
          space_separated_must_exclude.data[idx] == ' ')
      {
        i32 size = idx - last_wordstart;
        if(size > 0)
        {
          str_t word = {
            .size = (u32)size,
            .data = space_separated_must_exclude.data + last_wordstart,
          };
          wordlink_t* new_excluded_word = alloc_struct(arena, wordlink_t);
          new_excluded_word->word = word;
          new_excluded_word->next = excluded_words;
          excluded_words = new_excluded_word;
        }
        last_wordstart = idx + 1;
      }
    }

    // Find words that could fit into the input.
    keylink_t* subkeys = 0;
    for(u32 entry_idx = 0;
        entry_idx < array_count(hashtable->entries);
        ++entry_idx)
    {
      for(keylink_t* key_link = hashtable->entries[entry_idx];
          key_link;
          key_link = key_link->next)
      {
        if(breakdown_contains(&reduced_input_breakdown, &key_link->key))
        {
          wordlink_t* current_wordlink = 0;

          for(wordlink_t* test_wordlink = &key_link->first_word;
              test_wordlink;
              test_wordlink = test_wordlink->next)
          {
            str_t word = test_wordlink->word;

            b32 excluded = false;
            for(wordlink_t* excluded_word = excluded_words;
                excluded_word && !excluded;
                excluded_word = excluded_word->next)
            {
              if(str_eq(word, excluded_word->word))
              {
                excluded = true;
              }
            }

            if(!excluded)
            {
              if(!current_wordlink)
              {
                // Insert longest words first.
                u32 key_sum = breakdown_sum(&key_link->key);
                keylink_t** subkey = &subkeys;
                while(*subkey && breakdown_sum(&(*subkey)->key) >= key_sum)
                {
                  subkey = &(*subkey)->next;
                }

                keylink_t* new_subkey = alloc_struct(arena, keylink_t);
                new_subkey->key = key_link->key;
                new_subkey->next = *subkey;
                *subkey = new_subkey;
                current_wordlink = &new_subkey->first_word;
              }
              else
              {
                wordlink_t* new_word = alloc_struct(arena, wordlink_t);
                current_wordlink->next = new_word;
                current_wordlink = new_word;
              }

              current_wordlink->word = word;
              current_wordlink->next = 0;
            }
          }
        }
      }
    }

    if(subkeys)
    {
      u32 chain_max_length = max(1, breakdown_sum(input_breakdown));
      u32 chain_length = 0;
      keylink_t** chain = alloc_array(arena, chain_max_length, keylink_t*);

      breakdown_t remaining_breakdown = reduced_input_breakdown;
      chain[chain_length++] = subkeys;
      keylink_t* next_subkey_to_add = subkeys;
      b32 no_underflow = breakdown_subtract(&remaining_breakdown, &subkeys->key);
      assert(no_underflow);

      ctx.subkeys = subkeys;

      ctx.chain_max_length = chain_max_length;
      ctx.chain_length = chain_length;
      ctx.chain = chain;

      ctx.remaining_breakdown = remaining_breakdown;
      ctx.next_subkey_to_add = next_subkey_to_add;
    }
  }

  return ctx;
}

internal void end_anagram_context(anagram_context_t* ctx)
{
  if(ctx->initialized)
  {
    arena_restore(ctx->tmp_arena_snap);
    clear_arena(&ctx->results.arena);
    ctx->initialized = false;
  }
}

internal void compute_anagrams(anagram_context_t* ctx, u32 iterations)
{
  arena_t* arena = ctx->tmp_arena;
  u32 chain_max_length = ctx->chain_max_length;

  for(u32 iteration = 0;
      iteration < iterations && ctx->chain_length > 0;
      ++iteration)
  {
    if(ctx->next_subkey_to_add && breakdown_is_empty(&ctx->remaining_breakdown))
    {
      // Print results, with per-word anagram combinations.
      arena_snap_t snap = arena_snap(arena);

      wordlink_t** tmp_links = alloc_array(arena, ctx->chain_length, wordlink_t*);
      for(u32 link_idx = 0;
          link_idx < ctx->chain_length;
          ++link_idx)
      {
        tmp_links[link_idx] = &ctx->chain[link_idx]->first_word;
      }

      for(;;)
      {
        anagram_result_t* result = begin_anagram_result(&ctx->results, ctx->chain_length);
        for(u32 link_idx = 0;
            link_idx < ctx->chain_length;
            ++link_idx)
        {
          result->words[link_idx] = tmp_links[link_idx]->word;
        }

        // Go to next per-word anagram permutation.
        tmp_links[0] = tmp_links[0]->next;
        for(u32 link_idx = 0;
            link_idx < ctx->chain_length - 1;
            ++link_idx)
        {
          if(!tmp_links[link_idx])
          {
            tmp_links[link_idx] = &ctx->chain[link_idx]->first_word;
            tmp_links[link_idx + 1] = tmp_links[link_idx + 1]->next;
          }
        }
        if(!tmp_links[ctx->chain_length - 1])
        {
          ctx->next_subkey_to_add = 0;
          break;
        }
      }

      arena_restore(snap);
    }
    else if(ctx->next_subkey_to_add)
    {
      assert(!breakdown_underflowed(&ctx->remaining_breakdown));
      // Try adding a new chain element.
      assert(ctx->chain_length < chain_max_length);
      ctx->chain[ctx->chain_length++] = ctx->next_subkey_to_add;
      if(breakdown_subtract(&ctx->remaining_breakdown, &ctx->next_subkey_to_add->key))
      {
        ctx->next_subkey_to_add = ctx->chain[ctx->chain_length - 1];
      }
      else
      {
        ctx->next_subkey_to_add = 0;
      }
    }
    else
    {
      // Try changing the last chain element.
      keylink_t* prev_last_subkey = ctx->chain[--ctx->chain_length];
      breakdown_add(&ctx->remaining_breakdown, &prev_last_subkey->key);
      keylink_t* next_subkey = prev_last_subkey->next;
      if(next_subkey)
      {
        breakdown_t* next_key = &next_subkey->key;
        assert(ctx->chain_length < chain_max_length);
        ctx->chain[ctx->chain_length++] = next_subkey;
        if(breakdown_subtract(&ctx->remaining_breakdown, next_key))
        {
          ctx->next_subkey_to_add = ctx->chain[ctx->chain_length - 1];
        }
        else
        {
          ctx->next_subkey_to_add = 0;
        }
      }
      else
      {
        ctx->next_subkey_to_add = 0;
      }
    }
  }

  ctx->results.not_done = (ctx->chain_length > 0);
}

internal b32 delete_substring(str_t* str, size_t start, size_t count)
{
  b32 changed = false;

  start = min(str->size, start);
  count = min(str->size - start, count);

  if(count > 0)
  {
    for(size_t char_idx = (size_t)start;
        char_idx < str->size - count;
        ++char_idx)
    {
      str->data[char_idx] = str->data[char_idx + count];
    }
    str->size -= count;
    changed = true;
  }

  return changed;
}

internal size_t find_previous_word_boundary(str_t* str, size_t start)
{
  size_t result = min(str->size, start);

  b32 encountered_nonspace = false;
  while(result > 0)
  {
    encountered_nonspace |= (str->data[result - 1] != ' ');
    --result;
    if(encountered_nonspace && result > 0 && str->data[result - 1] == ' ')
    {
      break;
    }
  }

  return result;
}

internal size_t find_next_word_boundary(str_t* str, size_t start)
{
  size_t result = min(str->size, start);

  b32 encountered_nonspace = false;
  while(result < str->size)
  {
    encountered_nonspace |= (str->data[result] != ' ');
    ++result;
    if(encountered_nonspace && result < str->size && str->data[result] == ' ')
    {
      break;
    }
  }

  return result;
}

internal void find_boundaries_around_word(str_t* str, size_t around, size_t* start, size_t* end)
{
  *start = find_previous_word_boundary(str, around + 1);
  *end = find_next_word_boundary(str, around);

  while(*start > 0 && str->data[*start - 1] == ' ')
  {
    --*start;
  }
  if(*start == 0)
  {
    while(*end < str->size && str->data[*end] == ' ')
    {
      ++*end;
    }
  }
}

typedef enum
{
  UI_STR_INPUT,
  UI_STR_INCLUDE,
  UI_STR_EXCLUDE,

  UI_STR_COUNT,
} ui_str_idx_t;

typedef struct
{
  str_t ui_strs[UI_STR_COUNT];

  ui_str_idx_t active_ui_str_idx;
  i32 cursor_pos;
  i32 skip_results;
  i32 skip_results_target;

  b32 show_help;
  i32 help_expansion;
  b32 help_visible;

  b32 show_debug;
} ui_state_t;

#define MAX_USER_INPUT_SIZE 1024

typedef struct undo_entry_t
{
  ui_state_t ui_state;  // TODO: Leave out unrecorded fields.

  arena_snap_t arena_after_this;
  
  struct undo_entry_t* previous;
  struct undo_entry_t* next;
} undo_entry_t;

typedef struct
{
  arena_t* arena;

  undo_entry_t* first_entry;
  undo_entry_t* current_entry;
  undo_entry_t* last_entry;
} undo_history_t;

internal void scroll_results(ui_state_t* state, i32 scroll_amount)
{
  i32 prev_skip_results = state->skip_results_target;
  state->skip_results_target = max(0, prev_skip_results + scroll_amount);
}

internal void handle_ui_str_deletion(ui_state_t* state,
    ui_str_idx_t str_idx_to_delete_from,
    b32* inputs_changed, size_t deletion_start, size_t deletion_end)
{
  if(deletion_start < deletion_end)
  {
    str_t* str = state->ui_strs + str_idx_to_delete_from;
    size_t deletion_length = deletion_end - deletion_start;
    delete_substring(str, deletion_start, deletion_length);
    *inputs_changed = true;
    if(state->active_ui_str_idx == str_idx_to_delete_from)
    {
      if(state->cursor_pos > deletion_end)
      {
        state->cursor_pos -= deletion_length;
      }
      else if(state->cursor_pos > deletion_start)
      {
        state->cursor_pos = deletion_start;
      }
    }
  }
}

internal void copy_str_unsafe(str_t src, str_t* dst)
{
  dst->size = src.size;
  for(size_t char_idx = 0;
      char_idx < src.size;
      ++char_idx)
  {
    dst->data[char_idx] = src.data[char_idx];
  }
}

internal b32 record_for_undo(ui_state_t* state, undo_history_t* history)
{
  b32 do_record = false;

  // TODO: Clear old undo entries if history uses too much memory.

  if(!history->current_entry)
  {
    do_record = true;
  }
  else
  {
    undo_entry_t* previous_entry = history->current_entry;
    ui_state_t* previous_state = &previous_entry->ui_state;

    for(ui_str_idx_t str_idx = 0;
        str_idx < UI_STR_COUNT;
        ++str_idx)
    {
      do_record |= !str_eq(state->ui_strs[str_idx], previous_state->ui_strs[str_idx]);
    }
  }

  if(do_record)
  {
    if(history->current_entry)
    {
      arena_restore(history->current_entry->arena_after_this);

      // Not strictly necessary, but avoid pointing into freed memory.
      history->current_entry->next = 0;
      history->last_entry = history->current_entry;
    }

    undo_entry_t* new_entry = alloc_struct_clear(history->arena, undo_entry_t);
    new_entry->previous = history->current_entry;
    if(!history->current_entry)
    {
      history->first_entry = new_entry;
      new_entry->previous = 0;
    }
    else
    {
      history->current_entry->next = new_entry;
    }
    history->last_entry = new_entry;
    history->current_entry = new_entry;

    new_entry->ui_state = *state;

    for(ui_str_idx_t str_idx = 0;
        str_idx < UI_STR_COUNT;
        ++str_idx)
    {
      str_t src_str = state->ui_strs[str_idx];
      str_t* dst_str = &new_entry->ui_state.ui_strs[str_idx];

      dst_str->data = alloc_array(history->arena, src_str.size, u8);
      copy_str_unsafe(src_str, dst_str);
    }

    new_entry->arena_after_this = arena_snap(history->arena);
  }

  return do_record;
}

internal void apply_undo_entry_to_state(ui_state_t* state, undo_entry_t* entry)
{
  for(ui_str_idx_t str_idx = 0;
      str_idx < UI_STR_COUNT;
      ++str_idx)
  {
    copy_str_unsafe(entry->ui_state.ui_strs[str_idx], &state->ui_strs[str_idx]);
  }

  state->active_ui_str_idx = entry->ui_state.active_ui_str_idx;
  state->cursor_pos = entry->ui_state.cursor_pos;
  state->skip_results = entry->ui_state.skip_results;
}

internal b32 undo(ui_state_t* state, undo_history_t* history)
{
  b32 result = false;

  record_for_undo(state, history);
  if(history->current_entry && history->current_entry->previous)
  {
    apply_undo_entry_to_state(state, history->current_entry->previous);
    history->current_entry = history->current_entry->previous;
    result = true;
  }

  return result;
}

internal b32 redo(ui_state_t* state, undo_history_t* history)
{
  b32 result = false;

  if(history->current_entry->next)
  {
    history->current_entry = history->current_entry->next;
    apply_undo_entry_to_state(state, history->current_entry);
    result = true;
  }

  return result;
}

internal void go_live(hashtable_t* hashtable)
{
  terminal_context_t terminal_context;
  begin_terminal_io(&terminal_context);
  char_frame_t frame = {0};
  live_input_t input = {0};

  arena_t tmp_arena = new_custom_arena(512 * 1024);
  u8* input_buf   = alloc_array(&tmp_arena, MAX_USER_INPUT_SIZE, u8);
  u8* include_buf = alloc_array(&tmp_arena, MAX_USER_INPUT_SIZE, u8);
  u8* exclude_buf = alloc_array(&tmp_arena, MAX_USER_INPUT_SIZE, u8);
  u8* previous_exclude_buf = alloc_array(&tmp_arena, MAX_USER_INPUT_SIZE, u8);

  ui_state_t* state = &(ui_state_t){0};
  state->ui_strs[UI_STR_INPUT].data   = input_buf;
  state->ui_strs[UI_STR_INCLUDE].data = include_buf;
  state->ui_strs[UI_STR_EXCLUDE].data = exclude_buf;

  arena_t undo_arena = new_custom_arena(256 * 1024);
  undo_history_t* history = alloc_struct_clear(&undo_arena, undo_history_t);
  history->arena = &undo_arena;

  anagram_context_t anagram_context = {0};
  b32 dirty = true;
  b32 inputs_changed = false;
  u32 frame_count = 0;

  record_for_undo(state, history);

  while(!global_quitting)
  {
    get_terminal_events(&terminal_context, &input, &frame);
    if(global_quitting) { break; }
    v2i mouse_pos = input.mouse_pos;
    b32 left_clicked = went_down(input.btn_mouse_left);
    b32 mouse_left_down = ended_down(input.btn_mouse_left);
    b32 right_clicked = went_down(input.btn_mouse_right);
    dirty |= frame.full_redraw;

    i32 anagram_start_y = frame.height - 12;
    i32 visible_anagram_count = anagram_start_y + 1;

    breakdown_t previous_input_breakdown = breakdown_word(state->ui_strs[UI_STR_INPUT]);
    breakdown_t previous_include_breakdown = breakdown_word(state->ui_strs[UI_STR_INCLUDE]);
    str_t previous_exclude = {0, previous_exclude_buf};
    copy_str_unsafe(state->ui_strs[UI_STR_EXCLUDE], &previous_exclude);
    i32 previous_cursor_pos = state->cursor_pos;
    i32 previous_skip_results = state->skip_results;
    undo_entry_t* previous_current_undo_entry = history->current_entry;
    for(u32 typed_key_idx = 0;
        typed_key_idx < input.typed_key_count;
        ++typed_key_idx)
    {
      u8 typed_key = input.typed_keys[typed_key_idx];
      str_t* active_str = state->ui_strs + state->active_ui_str_idx;
      switch(typed_key)
      {
        case KEY_ARROW_LEFT:
        {
          if(state->cursor_pos > 0)
          {
            record_for_undo(state, history);
            --state->cursor_pos;
          }
        } break;

        case KEY_ARROW_RIGHT:
        {
          if(state->cursor_pos < active_str->size)
          {
            record_for_undo(state, history);
            ++state->cursor_pos;
          }
        } break;

        case KEY_CTRL_ARROW_LEFT:
        {
          record_for_undo(state, history);
          state->cursor_pos = find_previous_word_boundary(active_str, state->cursor_pos);
        } break;

        case KEY_CTRL_ARROW_RIGHT:
        {
          record_for_undo(state, history);
          state->cursor_pos = find_next_word_boundary(active_str, state->cursor_pos);
        } break;

        case KEY_ARROW_DOWN:
        {
          record_for_undo(state, history);
          scroll_results(state, 1);
        } break;

        case KEY_ARROW_UP:
        {
          record_for_undo(state, history);
          scroll_results(state, -1);
        } break;

        case KEY_PAGE_DOWN:
        case KEY_CTRL_PAGE_DOWN:
        {
          record_for_undo(state, history);
          scroll_results(state, max(1, visible_anagram_count - 2));
        } break;

        case KEY_PAGE_UP:
        case KEY_CTRL_PAGE_UP:
        {
          record_for_undo(state, history);
          scroll_results(state, -max(1, visible_anagram_count - 2));
        } break;

        case KEY_CTRL_HOME:
        {
          record_for_undo(state, history);
          state->skip_results_target = 0;
        } break;

        case KEY_CTRL_END:
        {
          record_for_undo(state, history);
          state->skip_results_target = max(0, anagram_context.results.result_count - visible_anagram_count);
          if(anagram_context.results.not_done)
          {
            state->skip_results_target += visible_anagram_count / 2;
          }
        } break;

        case KEY_CTRL_A:
        case KEY_HOME:
        {
          record_for_undo(state, history);
          state->cursor_pos = 0;
        } break;

        case KEY_CTRL_E:
        case KEY_END:
        {
          record_for_undo(state, history);
          state->cursor_pos = active_str->size;
        } break;

        case KEY_TAB:
        case KEY_SHIFT_TAB:
        case KEY_ENTER:
        {
          record_for_undo(state, history);
          if(typed_key == KEY_SHIFT_TAB)
          {
            --state->active_ui_str_idx;
            if((i32)state->active_ui_str_idx < 0)
            {
              state->active_ui_str_idx += UI_STR_COUNT;
            }
          }
          else
          {
            state->active_ui_str_idx = (state->active_ui_str_idx + 1) % UI_STR_COUNT;
          }
          state->cursor_pos = state->ui_strs[state->active_ui_str_idx].size;
          dirty = true;
        } break;

        case KEY_CTRL_K:
        {
          record_for_undo(state, history);
          dirty |= delete_substring(active_str, state->cursor_pos, active_str->size - state->cursor_pos);
        } break;

        case KEY_CTRL_U:
        {
          record_for_undo(state, history);
          dirty |= delete_substring(active_str, 0, state->cursor_pos);
          state->cursor_pos = 0;
        } break;

        case KEY_CTRL_W:
        case KEY_CTRL_BACKSPACE:
        {
          record_for_undo(state, history);
          i32 orig_cursor_pos = state->cursor_pos;
          state->cursor_pos = find_previous_word_boundary(active_str, state->cursor_pos);
          i32 offset = orig_cursor_pos - state->cursor_pos;
          dirty |= delete_substring(active_str, state->cursor_pos, offset);
        } break;

        case KEY_ALT_D:
        case KEY_CTRL_DELETE:
        {
          record_for_undo(state, history);
          i32 offset = find_next_word_boundary(active_str, state->cursor_pos) - state->cursor_pos;
          dirty |= delete_substring(active_str, state->cursor_pos, offset);
        } break;

        case KEY_BACKSPACE:
        {
          // TODO: record_for_undo if this is the first backspace event in sequence?
          if(state->cursor_pos > 0)
          {
            --state->cursor_pos;
            dirty |= delete_substring(active_str, state->cursor_pos, 1);
          }
        } break;

        case KEY_DELETE:
        {
          if(state->cursor_pos < active_str->size)
          {
            dirty |= delete_substring(active_str, state->cursor_pos, 1);
          }
        } break;

#if 1
        // DEBUG
        case KEY_CTRL_S:
        {
          dirty |= record_for_undo(state, history);
        } break;
#endif

        case KEY_CTRL_O:
        case KEY_CTRL_Z:
        {
          dirty |= undo(state, history);
        } break;

        case KEY_CTRL_Y:
        {
          dirty |= redo(state, history);
        } break;

        case KEY_F1:
        case KEY_CTRL_SLASH:
        {
          state->show_help = !state->show_help;
          if(state->show_help && state->help_expansion == 0)
          {
            state->help_expansion = 1;
          }
          dirty = true;
        } break;

        case KEY_F12:
        {
          state->show_debug = !state->show_debug;
          dirty = true;
        } break;

        default:
        {
          if(is_printable(typed_key))
          {
            if(active_str->size < MAX_USER_INPUT_SIZE)
            {
              if(state->cursor_pos >= 1 &&
                  typed_key == ' ' &&
                  active_str->data[state->cursor_pos - 1] != ' ')
              {
                record_for_undo(state, history);
              }

              for(i32 char_idx = active_str->size;
                  char_idx > state->cursor_pos;
                  --char_idx)
              {
                active_str->data[char_idx] = active_str->data[char_idx - 1];
              }
              ++active_str->size;
              active_str->data[state->cursor_pos++] = typed_key;
              dirty = true;
            }
          }
        } break;
      }
    }

    scroll_results(state, -2 * input.mouse_scroll_y);

    if(!anagram_context.results.not_done)
    {
      state->skip_results_target =
        min(anagram_context.results.result_count - 1, state->skip_results_target);
    }

    if(state->skip_results_target < state->skip_results)
    {
      state->skip_results -= (state->skip_results - state->skip_results_target + 3) / 4;
    }
    else if(state->skip_results_target > state->skip_results)
    {
      state->skip_results += (state->skip_results_target - state->skip_results + 3) / 4;
    }

    if(left_clicked || mouse_left_down || right_clicked)
    {
      record_for_undo(state, history);
    }

    {
      breakdown_t input_breakdown = breakdown_word(state->ui_strs[UI_STR_INPUT]);
      breakdown_t include_breakdown = breakdown_word(state->ui_strs[UI_STR_INCLUDE]);
      inputs_changed |= !breakdown_eq(&previous_input_breakdown, &input_breakdown);
      inputs_changed |= !breakdown_eq(&previous_include_breakdown, &include_breakdown);
      inputs_changed |= !str_eq(previous_exclude, state->ui_strs[UI_STR_EXCLUDE]);
    }

    dirty |= left_clicked;
    dirty |= mouse_left_down;
    dirty |= right_clicked;
    dirty |= (state->cursor_pos != previous_cursor_pos);
    dirty |= (state->skip_results != previous_skip_results);
    dirty |= (state->show_debug && history->current_entry != previous_current_undo_entry);
    dirty |= inputs_changed;
    dirty |= anagram_context.results.not_done;
    if(dirty)
    {
      dirty = false;

      for(u32 y = 0;
          y < frame.height;
          ++y)
      {
        for(u32 x = 0;
            x < frame.width;
            ++x)
        {
          frame.chars[frame.width * y + x] = (color_char_t){0};
        }
      }

      if(state->show_debug)
      {
        b32 current_entry_found = false;
        u32 current_undo_idx = 0;
        u32 undo_count = 0;
        for(undo_entry_t* entry = history->first_entry;
            entry;
            entry = entry->next)
        {
          if(!current_entry_found && entry == history->current_entry)
          {
            current_undo_idx = undo_count;
            current_entry_found = true;
          }
          ++undo_count;
        }

        u8 txt[256];
        size_t len = snprintf(txt, 256,
            "Tmp arena: %uK; Results arena: %uM; Undo history: %u/%u, %uK",
            (u32)(tmp_arena.total_capacity / 1024),
            (u32)(anagram_context.results.arena.total_capacity / 1024 / 1024),
            current_undo_idx + 1, undo_count, (u32)(undo_arena.total_capacity / 1024));
        str_t str = {len, txt};
        draw_str(&frame, (v3u8){0, 255, 0}, black, 0, frame.height - 1, str);
      }

      i32 start_x = 2;
      i32 end_x = frame.width - 2;
      str_t ui_str_labels[] = {
        str("Input:"),
        str("Include:"),
        str("Exclude:"),
      };

      {
        i32 max_str_size = end_x - start_x;
        breakdown_t input_remaining = breakdown_word(state->ui_strs[UI_STR_INPUT]);
        breakdown_t include_remaining = breakdown_word(state->ui_strs[UI_STR_INCLUDE]);
        i32 prev_active_ui_str_idx = state->active_ui_str_idx;
        for(i32 pass = 0;
            pass <= 1;
            ++pass)
        {
          for(ui_str_idx_t ui_str_idx = 0;
              ui_str_idx < UI_STR_COUNT;
              ++ui_str_idx)
          {
            i32 y = frame.height - 3 - 3 * ui_str_idx;

            b32 active = (ui_str_idx == state->active_ui_str_idx);
            str_t* ui_str = state->ui_strs + ui_str_idx;
            str_t drawn_str;
            drawn_str.size = min(max_str_size - 1, ui_str->size);
            i32 drawn_str_offset = ui_str->size - drawn_str.size;
            if(active)
            {
              drawn_str_offset = max(0, min(state->cursor_pos - max_str_size/2, drawn_str_offset));
            }
            drawn_str.data = ui_str->data + drawn_str_offset;

            if(pass == 0)
            {
              if(mouse_pos.y >= y - 1 && mouse_pos.y <= y + 1)
              {
                i32 clicked_at_char = max(0, min((i32)ui_str->size,
                      mouse_pos.x + drawn_str_offset - start_x));

                if(left_clicked || mouse_left_down)
                {
                  state->active_ui_str_idx = ui_str_idx;
                  state->cursor_pos = clicked_at_char;
                }

                if(right_clicked)
                {
                  clicked_at_char = min((i32)ui_str->size - 1, clicked_at_char);
                  if(ui_str->data[clicked_at_char] != ' ')
                  {
                    size_t deletion_start = 0;
                    size_t deletion_end = 0;
                    find_boundaries_around_word(ui_str, clicked_at_char,
                        &deletion_start, &deletion_end);

                    handle_ui_str_deletion(state, ui_str_idx,
                        &inputs_changed, deletion_start, deletion_end);
                  }
                }
              }
            }
            else
            {
              draw_str(&frame, white, black, start_x, y + 1, ui_str_labels[ui_str_idx]);

              for(i32 i = 0;
                  i < max_str_size;
                  ++i)
              {
                i32 x = start_x + i;
                u8 c = (i < drawn_str.size) ? drawn_str.data[i] : ' ';

                b32 dim = false;
                b32 warning = false;
                if(ui_str_idx == UI_STR_INPUT)
                {
                  if(is_alpha(c))
                  {
                    u8 k = to_lower(c) - 'a';
                    dim |= (include_remaining.counts[k] > 0);
                    --include_remaining.counts[k];
                  }
                }
                else if(ui_str_idx == UI_STR_INCLUDE)
                {
                  if(is_alpha(c))
                  {
                    u8 k = to_lower(c) - 'a';
                    warning |= (input_remaining.counts[k] <= 0);
                    --input_remaining.counts[k];
                  }
                }

                v3u8 fg_col = warning ? bright_red : (dim ? bright_gray : white);
                v3u8 bg_col = black;
                if(active && ((i + drawn_str_offset) != state->cursor_pos))
                {
                  fg_col = warning ? dark_red : (dim ? dark_gray : black);
                  bg_col = white;
                }

                draw_char(&frame, fg_col, bg_col, x, y, c);
              }
            }
          }
        }
        if(state->active_ui_str_idx != prev_active_ui_str_idx)
        {
          dirty = true;
        }
      }

      if(inputs_changed)
      {
        state->skip_results = 0;
        state->skip_results_target = 0;
        end_anagram_context(&anagram_context);
        dirty = true;  // update arena statistics in debug view

        breakdown_t input_breakdown = breakdown_word(state->ui_strs[UI_STR_INPUT]);
        breakdown_t must_include_breakdown = breakdown_word(state->ui_strs[UI_STR_INCLUDE]);
        anagram_context = begin_anagram_context(hashtable, &tmp_arena,
            &input_breakdown, &must_include_breakdown, state->ui_strs[UI_STR_EXCLUDE]);

        inputs_changed = false;
      }

      if(anagram_context.results.result_count < state->skip_results + visible_anagram_count + 100)
      {
        compute_anagrams(&anagram_context, 100000);
      }

      anagram_results_t* results = &anagram_context.results;
      {
        u8 txt[256];
        size_t len = 0;
        if(results->result_count > 0)
        {
          u32 count_len = snprintf(txt, array_count(txt), "%u", (u32)results->result_count);
          count_len = max(4, count_len);
          len = snprintf(txt, array_count(txt), "Results %*u to %*u of %*u%s:",
              count_len, (u32)(state->skip_results + 1),
              count_len, (u32)(max(state->skip_results + 1,
                                   min(results->result_count,
                                       state->skip_results + max(1, visible_anagram_count)))),
              count_len, (u32)results->result_count, results->not_done ? " (maybe more)" : "");
        }
        else
        {
          len = snprintf(txt, array_count(txt), "No results.");
        }
        str_t str = {len, txt};
        i32 x = start_x;
        i32 y = anagram_start_y + 1;
        draw_str(&frame, white, black, x, y, str);
      }
      i32 current_y = anagram_start_y + state->skip_results;
      str_t orig_include_str = state->ui_strs[UI_STR_INCLUDE];
      size_t include_deletion_start = 0;
      size_t include_deletion_end = 0;
      for(anagram_result_t* result = results->first_result;
          result && current_y >= 0;
          result = result->next_result)
      {
        if(current_y <= anagram_start_y)
        {
          i32 current_x = start_x + 2;
          if(orig_include_str.size > 0)
          {
            if(left_clicked || right_clicked)
            {
              i32 clicked_at_char = mouse_pos.x - current_x;
              if(clicked_at_char >= 0 && clicked_at_char < orig_include_str.size &&
                  orig_include_str.data[clicked_at_char] != ' ' &&
                  mouse_pos.y == current_y)
              {
                find_boundaries_around_word(&state->ui_strs[UI_STR_INCLUDE], clicked_at_char,
                    &include_deletion_start, &include_deletion_end);
              }
            }

            current_x += 1 + draw_str(&frame, bright_gray, black, current_x, current_y, orig_include_str);
          }

          for(u32 word_idx = 0;
              word_idx < result->word_count;
              ++word_idx)
          {
            str_t word = result->words[word_idx];

            if(word_idx > 0) { current_x += 1; }

            if(mouse_pos.x >= current_x && mouse_pos.x <= current_x + word.size &&
                mouse_pos.y == current_y)
            {
              if(left_clicked)
              {
                // TODO: Defer mouse actions, check that click was not occluded by help.
                str_t* include_str = state->ui_strs + UI_STR_INCLUDE;
                if(include_str->size + word.size + 1 <= MAX_USER_INPUT_SIZE)
                {
                  if(include_str->size > 0 && include_str->data[include_str->size - 1] != ' ')
                  {
                    include_str->data[include_str->size++] = ' ';
                  }

                  for(u32 char_idx = 0;
                      char_idx < word.size;
                      ++char_idx)
                  {
                    include_str->data[include_str->size++] = word.data[char_idx];
                  }

                  if(state->active_ui_str_idx == UI_STR_INCLUDE &&
                      state->cursor_pos == orig_include_str.size)
                  {
                    state->cursor_pos = include_str->size;
                  }

                  inputs_changed = true;
                }
              }
              else if(right_clicked)
              {
                str_t* exclude_str = state->ui_strs + UI_STR_EXCLUDE;
                if(exclude_str->size + word.size + 1 <= MAX_USER_INPUT_SIZE)
                {
                  if(exclude_str->size > 0 && exclude_str->data[exclude_str->size - 1] != ' ')
                  {
                    exclude_str->data[exclude_str->size++] = ' ';
                  }

                  for(u32 char_idx = 0;
                      char_idx < word.size;
                      ++char_idx)
                  {
                    exclude_str->data[exclude_str->size++] = word.data[char_idx];
                  }

                  if(state->active_ui_str_idx == UI_STR_EXCLUDE)
                  {
                    state->cursor_pos = exclude_str->size;
                  }

                  inputs_changed = true;
                }
              }
            }

            current_x += draw_str(&frame, white, black, current_x, current_y, word);
          }
        }
        --current_y;
      }

      handle_ui_str_deletion(state, UI_STR_INCLUDE, &inputs_changed,
          include_deletion_start, include_deletion_end);

      if(results->not_done)
      {
        u8 status_chars[] = "... searching ...";
        str_t status_str = str(status_chars);
        u32 anim_phase = (frame_count / 5) % (2 * status_str.size - 2);
        if(anim_phase >= status_str.size)
        {
          anim_phase = 2 * status_str.size - anim_phase - 2;
        }
        u8* c = status_str.data + anim_phase;
        if(*c == '.')
        {
          *c = '?';
        }
        else if(*c == ' ')
        {
          *c = '?';
        }
        else if(*c >= 'a' && *c <= 'z')
        {
          *c = to_upper(*c);
        }
        i32 searching_y = min(anagram_start_y, current_y);
        draw_str(&frame, bright_gray, black, start_x + 2, searching_y, status_str);
      }

      if(state->help_expansion > 0)
      {
        str_t help_lines[] = {
          str("                    ---  KEYS  ---                    "),
          str(""),
          str("F1, Ctrl+/                  Toggle this help"),
          str("Tab, Shift+Tab, Enter       Cycle through input fields"),
          str("Scroll, Up/Down, PgUp/PgDn  Scroll through results"),
          str("Ctrl+Home, Ctrl+End         Jump to results start, end"),
          str("Left click on result        Add word to inclusions"),
          str("Right click on result       Add word to exclusions"),
          str("Right click on input        Delete word"),
          str("Ctrl+U, Ctrl+K              Delete to start, end"),
          str("Ctrl+W, Alt+D               Delete word to left, right"),
          str("Ctrl+Z, Ctrl+Y              Undo, redo"),
        };

        i32 help_line_count = array_count(help_lines);
        i32 help_width = 58;
        i32 help_height = help_line_count + 2;

        i32 help_left_x = (frame.width - help_width) / 2;
        i32 help_top_y =
          frame.height - 1 - (frame.height - help_height) / 2;
        i32 help_bottom_y = help_top_y - state->help_expansion + 1;

        if(state->show_help)
        {
          if(state->help_expansion < help_height)
          {
            ++state->help_expansion;
            dirty = true;
          }
        }
        else
        {
          --state->help_expansion;
          dirty = true;
        }

        v3u8 help_fg = white;
        v3u8 help_bg = dark_gray;

        for(i32 y = help_top_y;
            y >= help_bottom_y;
            --y)
        {
          for(i32 x = help_left_x;
              x < help_left_x + help_width;
              ++x)
          {
            draw_char(&frame, help_fg, help_bg, x, y, ' ');
          }
        }

        for(i32 line_idx = 0;
            line_idx < help_line_count;
            ++line_idx)
        {
          i32 x = help_left_x + 2;
          i32 y = help_top_y - 1 - line_idx;
          if(y < help_bottom_y) { break; }
          draw_str(&frame, help_fg, help_bg, x, y, help_lines[line_idx]);
        }
      }

      print_frame(&terminal_context, &frame);

      ++frame_count;
    }

    usleep(20000);
  }

  end_terminal_io(&terminal_context);
}

int main(int argument_count, char** arguments)
{
  counted_args_t* args = &(counted_args_t){argument_count, arguments};
  char* progname = pop_arg(args);

  b32 include_uppercase = false;
  if(args->count && zstr_eq(args->values[0], "--upper"))
  {
    pop_arg(args);
    include_uppercase = true;
  }

  char* wordfile_path = "data/words.txt";
  if(args->count >= 2 && zstr_eq(args->values[0], "--dict"))
  {
    pop_arg(args);
    wordfile_path = pop_arg(args);
  }
  str_t wordfile_contents = read_file(wordfile_path);

  arena_t hash_arena = new_arena();

  if(wordfile_contents.size)
  {
    hashtable_t* hashtable = alloc_struct_clear(&hash_arena, hashtable_t);

    // Build hash.
    u8* wordfile_past_end = wordfile_contents.data + wordfile_contents.size;
    u8* cursor = wordfile_contents.data;
    u8* word_start = cursor;
    b32 word_valid = true;
    while(cursor <= wordfile_past_end)
    {
      if(cursor == wordfile_past_end || is_linebreak(*cursor))
      {
        i32 word_length = cursor - word_start;
        if(word_valid)
        {
          str_t word = {word_length, word_start};
          breakdown_t breakdown = breakdown_word(word);
          if(breakdown_sum(&breakdown) > 0)
          {
            hashtable_add_word(hashtable, &hash_arena, word, &breakdown);
          }
        }
        word_valid = true;
        word_start = cursor + 1;
      }
      else if(!is_ascii(*cursor) || (!include_uppercase && is_upper(*cursor)))
      {
        word_valid = false;
      }
      ++cursor;
    }

    if(args->count && zstr_eq(args->values[0], "--groups"))
    {
      pop_arg(args);

      i32 min_word_count = 10;
      if(args->count)
      {
        min_word_count = atoi(pop_arg(args));
      }

      // List words with the most single-word anagrams.
      arena_t tmp_arena = new_arena();
      list_anagram_groups(hashtable, &tmp_arena, min_word_count);
    }
    else
    {
      if(args->count && zstr_eq(args->values[0], "--repl"))
      {
        // Query user.
        u8 input[256];
        arena_t tmp_arena = new_arena();
        for(;;)
        {
          printf("\nQuery: ");
          fflush(stdout);
          if(fgets((char*)input, sizeof(input), stdin))
          {
            cursor = input;
            while(*cursor && !is_linebreak(*cursor))
            {
              ++cursor;
            }
            i32 word_length = cursor - input;
            if(word_length > 0)
            {
              str_t word = {word_length, input};

              breakdown_t input_breakdown = breakdown_word(word);
              list_anagrams_for(hashtable, &tmp_arena, input_breakdown, str(""), str(""), 20);
              clear_arena(&tmp_arena);
            }
          }
          else
          {
            break;
          }
        }
      }
      else if(args->count && !zstr_eq(args->values[0], "--live"))
      {
        str_t input = wrap_str(pop_arg(args));

        str_t must_include = {0};
        if(args->count)
        {
          must_include = wrap_str(pop_arg(args));
        }

        str_t must_exclude = {0};
        if(args->count)
        {
          must_exclude = wrap_str(pop_arg(args));
        }

        breakdown_t input_breakdown = breakdown_word(input);
        arena_t tmp_arena = new_arena();
        list_anagrams_for(hashtable, &tmp_arena, input_breakdown, must_include, must_exclude, -1);
      }
      else
      {
        go_live(hashtable);
      }
    }
  }

  return 0;
}
