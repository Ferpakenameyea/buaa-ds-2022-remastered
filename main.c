#include <stdlib.h>
#include <memory.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>

#define timed \
    for (clock_t __start = clock(), __end = 0; __start != 0; __start = 0, printf("Elapsed: %.3f ms\n", (double)(__end - __start) * 1000.0 / CLOCKS_PER_SEC)) \
        for (__end = clock(); __end == 0; )


#pragma region math
static inline size_t make_align(size_t raw, size_t alignment)
{
    return (raw + (alignment - 1)) & ~(alignment - 1);
}

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) > (b) ? (b) : (a))

#pragma endregion

#pragma region memory

#define KB(kb_value) (1024 * (kb_value))
#define MB(mb_value) (1024 * 1024 * (mb_value))

typedef struct memblock_t {
    char* m_start;
    size_t m_size;
    struct memblock_t* m_next;
    bool m_is_on_heap;
} memblock;

typedef struct arena_allocator_t {
    void* (*allocate)(
        struct arena_allocator_t* self, 
        size_t size, 
        size_t align);

    struct memblock_t* m_first;
    struct memblock_t* m_current;
    size_t m_used;
} arena_allocator;

static inline void* arena_allocator_allocate(
    struct arena_allocator_t* self, 
    size_t request_size, 
    size_t alignment)
{
    self->m_used = make_align(self->m_used, alignment);

    request_start:
    const size_t available = self->m_current->m_size - self->m_used;

    if (available < request_size)
    {
        while (self->m_current->m_next != NULL)
        {
            self->m_current = self->m_current->m_next;
            // recursive
            goto request_start;
        }

        memblock* new_memblock = (memblock*)malloc(sizeof(memblock));
        const size_t new_size = MAX(self->m_current->m_size, request_size) * 2;
        (*new_memblock) = (memblock) {
            .m_start = (char*)malloc(new_size),
            .m_size = new_size,
            .m_next = NULL,
            .m_is_on_heap = true
        };

        printf("allocated new memblock");

        self->m_current->m_next = new_memblock;
        self->m_current = new_memblock;
        self->m_used = 0;
    }

    void* ptr = (void*)(self->m_current->m_start + self->m_used);
    self->m_used += request_size;

    return ptr;
}

static inline void init_arena_allocator(struct arena_allocator_t* self, memblock* init_block)
{
    self->allocate = arena_allocator_allocate;
    self->m_first = init_block;
    self->m_current = init_block;
    self->m_used = 0;
}

static inline void destroy_allocator(struct arena_allocator_t* self)
{
    memblock* mem = self->m_first;

    while (mem != NULL)
    {
        memblock* next = mem->m_next;
        if (mem->m_is_on_heap)
        {
            free(mem);
        }

        mem = next;
    }

    self->m_first = NULL;
    self->m_current = NULL;
    self->m_used = 0;
    self->allocate = NULL;
}

static inline void reset_allocator(struct arena_allocator_t* self) 
{
    self->m_current = self->m_first;
    self->m_used = 0;
}

#pragma endregion

#pragma region lifecycle
void program_construct();
void program_destruct();
int program_main(int argc, char** argv);
int main(int argc, char** argv)
{
    program_construct();
    int ret = program_main(argc, argv);
    program_destruct();
    return ret;
}

#pragma endregion

#pragma region hashmap_instance

#define ALIGN_HASHMAP_ENTRY sizeof(void*)
typedef struct hashmap_entry_t {
    const char* m_key; 
    struct hashmap_entry_t* m_next;
    unsigned int m_value;
} hashmap_entry;

typedef struct hashmap_t {
    struct hashmap_entry_t** m_entryies;
    struct hashmap_entry_t** m_buckets;
    
    size_t m_buckets_size;

    size_t m_entry_array_size;
    size_t m_entry_count;

    arena_allocator* m_entry_allocator;

    unsigned int (*m_hashcode_func)(const char* key, unsigned int length);
} hashmap;

void init_hashmap(
    struct hashmap_t* self,
    size_t entry_array_size, 
    size_t buckets_size, 
    unsigned int (*hashcode)(const char* key, unsigned int length),
    arena_allocator* allocator)
{
    // entries array doesn't need initialization
    self->m_entryies = (hashmap_entry**) malloc(sizeof(hashmap_entry*) * entry_array_size);
    self->m_buckets = (hashmap_entry**) calloc(buckets_size, sizeof(hashmap_entry*));

    self->m_entry_array_size = entry_array_size;
    self->m_buckets_size = buckets_size;

    self->m_entry_count = 0;
    self->m_entry_allocator = allocator;

    self->m_hashcode_func = hashcode;
}

static inline hashmap_entry* find(
    struct hashmap_t* self,
    const char* key,
    unsigned int key_length
) {
    unsigned int hashcode_value = self->m_hashcode_func(key, key_length);
    unsigned int bucket_index = hashcode_value % self->m_buckets_size;

    hashmap_entry* value = self->m_buckets[bucket_index];

    while (value != NULL && (strcmp(value->m_key, key) != 0))
    {
        value = value->m_next;
    }

    return value;
}

static inline void put_entry_to_array(struct hashmap_t* self, struct hashmap_entry_t* entry)
{
    if (self->m_entry_count == self->m_entry_array_size)
    {
        self->m_entry_array_size = self->m_entry_array_size * 2;
        self->m_entryies = realloc(
            self->m_entryies,
            self->m_entry_array_size * sizeof(hashmap_entry*));
    }

    self->m_entryies[self->m_entry_count] = entry;
    self->m_entry_count++;
}

static inline void put_or_update(
    struct hashmap_t* self,
    const char* key,
    int init_value,
    int (*update)(int original),
    unsigned int key_length
) {
    unsigned int hashcode_value = self->m_hashcode_func(key, key_length);
    unsigned int bucket_index = hashcode_value % self->m_buckets_size;

    hashmap_entry* value = self->m_buckets[bucket_index];

    if (value == NULL)
    {
        hashmap_entry* new_entry = self->m_entry_allocator->allocate(
            self->m_entry_allocator,
            sizeof(hashmap_entry),
            ALIGN_HASHMAP_ENTRY
        );

        new_entry->m_key = key;
        new_entry->m_value = init_value;
        new_entry->m_next = NULL;
        self->m_buckets[bucket_index] = new_entry;
        
        put_entry_to_array(self, new_entry);

        return;
    }

    while (true)
    {
        if (strcmp(value->m_key, key) == 0)
        {
            value->m_value = update(value->m_value);
            return;
        }

        if (value->m_next == NULL)
        {
            hashmap_entry* new_entry = self->m_entry_allocator->allocate(
                self->m_entry_allocator,
                sizeof(hashmap_entry),
                ALIGN_HASHMAP_ENTRY
            );

            new_entry->m_key = key;
            new_entry->m_value = init_value;
            new_entry->m_next = NULL;

            put_entry_to_array(self, new_entry);

            value->m_next = new_entry;
            return;
        }
        
        value = value->m_next;
    }
}

void destroy_hashmap(struct hashmap_t* self)
{
    free(self->m_buckets);
    free(self->m_entryies);
    
    self->m_buckets = NULL;
    self->m_buckets_size = 0;
    self->m_entry_allocator = NULL;
    self->m_entry_array_size = 0;
    self->m_entry_count = 0;
    self->m_entryies = NULL;
    self->m_hashcode_func = NULL;
}

static inline void reset_hashmap_and_allocator(struct hashmap_t* self) 
{
    memset(self->m_buckets, 0, self->m_buckets_size * sizeof(hashmap_entry*));
    self->m_entry_count = 0;
    reset_allocator(self->m_entry_allocator);
}

unsigned int murmur3_32(const char* key, unsigned int len) {
    unsigned int h = 0x9747b28c;
    const unsigned int c1 = 0xcc9e2d51;
    const unsigned int c2 = 0x1b873593;

    const int nblocks = len / 4;
    const unsigned int* blocks = (const unsigned int*) key;
    for (int i = 0; i < nblocks; i++) {
        unsigned int k = blocks[i];
        k *= c1;
        k = (k << 15) | (k >> (32 - 15));
        k *= c2;

        h ^= k;
        h = (h << 13) | (h >> (32 - 13));
        h = h*5 + 0xe6546b64;
    }

    const unsigned char* tail = (const unsigned char*)(key + nblocks*4);
    unsigned int k1 = 0;
    switch(len & 3) {
        case 3: k1 ^= tail[2] << 16;
        case 2: k1 ^= tail[1] << 8;
        case 1: k1 ^= tail[0];
                k1 *= c1; k1 = (k1 << 15) | (k1 >> (32 - 15)); k1 *= c2; h ^= k1;
    }

    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

#pragma endregion

#pragma region trie

#define ALIGN_TRIE_NODE sizeof(struct trie_node_t*)
typedef struct trie_node_t {
    bool m_is_terminal;
    struct trie_node_t* m_children[26];
} trie_node;

typedef struct trie_t {
    struct trie_node_t* m_roots[26];
    struct arena_allocator_t* m_allocator;
} trie;

static inline void put_trie(struct trie_t* self, const char* cstring)
{
    const char* current_char = cstring;
    trie_node* node = self->m_roots[(*current_char) - 'a'];

    if (node == NULL)
    {
        node = (trie_node*)self->m_allocator->allocate(
            self->m_allocator,
            sizeof(trie_node),
            ALIGN_TRIE_NODE
        );

        memset(node, 0, sizeof(trie_node));
        self->m_roots[(*current_char) - 'a'] = node;
    }
    current_char++;
    while ((*current_char) != '\0')
    {
        trie_node* next = node->m_children[(*current_char) - 'a'];
        if (next == NULL)
        {
            next = (trie_node*)self->m_allocator->allocate(
                self->m_allocator,
                sizeof(trie_node),
                ALIGN_TRIE_NODE
            );

            memset(next, 0, sizeof(trie_node));
            node->m_children[(*current_char) - 'a'] = next;
        }
        else 
        {
            node = next;
        }

        current_char++;
    }

    node->m_is_terminal = true;
}

static bool is_in_trie(struct trie_t* self, const char* cstring)
{
    const char* current_char = cstring;
    trie_node* node = self->m_roots[(*current_char) - 'a'];
    
    current_char++;
    
    while (node != NULL && (*current_char) != '\0')
    {
        node = node->m_children[(*current_char) - 'a'];
        current_char++;
    }

    return node != NULL && node->m_is_terminal;
}

static inline void init_trie(struct trie_t* self, struct arena_allocator_t* allocator)
{
    self->m_allocator = allocator;
    memset(self->m_roots, 0, sizeof(self->m_roots));
}

static inline void destroy_trie(struct trie_t* self)
{
    self->m_allocator = NULL;
}

#pragma endregion

typedef struct fingerprint_t {
    char* m_id;
    bool m_value[128];
} fingerprint;

#pragma region array_list_instance

typedef struct array_list_t {
    struct fingerprint_t* m_data;
    size_t m_capacity;
    size_t m_used;
} array_list;

static inline struct fingerprint_t* allocate_back(struct array_list_t* self)
{
    if (self->m_used == self->m_capacity)
    {
        self->m_capacity *= 2;
        self->m_data = realloc(self->m_data, self->m_capacity * sizeof(fingerprint));
    }

    struct fingerprint_t* allocated = &self->m_data[self->m_used];
    self->m_used++;

    return allocated;
}

#pragma endregion

// global_hashmap
char hashmap_memory[MB(10)];
memblock hashmap_entry_memblock;
arena_allocator hashmap_allocator;
hashmap global_hashmap;

// global_trie
char trie_memory[KB(240)];
memblock trie_node_memblock;
arena_allocator trie_allocator;
trie stopword_trie;

char words_memory[MB(20)];
memblock words_memblock;
arena_allocator words_allocator;

char name_memory[KB(20)];
memblock name_memblock;
arena_allocator name_allocator;

char hashvalue_multiplier[10000][128];

int vector_length;
int fingerprint_length;

array_list article_fingerprint_list;

bool sample_mode = false;

void program_construct()
{
    hashmap_entry_memblock = (memblock) {
        .m_start = hashmap_memory,
        .m_size = sizeof(hashmap_memory),
        .m_next = NULL,
        .m_is_on_heap = false
    };

    init_arena_allocator(&hashmap_allocator, &hashmap_entry_memblock);
    init_hashmap(&global_hashmap, 
        1024,
        2048,
        murmur3_32,
        &hashmap_allocator);

    trie_node_memblock = (memblock) {
        .m_start = trie_memory,
        .m_size = sizeof(trie_memory),
        .m_next = NULL,
        .m_is_on_heap = false
    };

    init_arena_allocator(&trie_allocator, &trie_node_memblock);
    init_trie(&stopword_trie, &trie_allocator);

    words_memblock = (memblock) {
        .m_start = words_memory,
        .m_size = sizeof(words_memory),
        .m_next = NULL,
        .m_is_on_heap = false
    };

    init_arena_allocator(&words_allocator, &words_memblock);

    name_memblock = (memblock) {
        .m_start = name_memory,
        .m_size = sizeof(name_memory),
        .m_next = NULL,
        .m_is_on_heap = false
    };

    init_arena_allocator(&name_allocator, &name_memblock);

    article_fingerprint_list = (array_list) {
        .m_data = (fingerprint*) malloc(sizeof(fingerprint) * 1024),
        .m_capacity = 1024,
        .m_used = 0
    };
}

void program_destruct()
{
    destroy_hashmap(&global_hashmap);
    destroy_trie(&stopword_trie);
    destroy_allocator(&hashmap_allocator);
    destroy_allocator(&trie_allocator);
}

int hashmap_update_func(int original)
{
    return original + 1;
}

static inline bool is_alpha(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

static inline char to_lower(char value)
{
    if (value >= 'A' && value <= 'Z')
    {
        return 'a' + (value - 'A');
    }

    return value;
}

static inline void get_stopwords()
{
    const char* input = "./stopwords.txt";
    FILE* file = fopen(input, "r");
    char buffer[50];
    while (fscanf(file, "%s", buffer) != EOF)
    {
        put_trie(&stopword_trie, buffer);
    }
    fclose(file);
}

static inline int compare_entries(const void* p1, const void* p2) {
    const hashmap_entry* entry1 = *(const hashmap_entry**) p1;
    const hashmap_entry* entry2 = *(const hashmap_entry**) p2;

    if (entry1->m_value == entry2->m_value) {
        return -strcmp(entry1->m_key, entry2->m_key);
    }

    return entry1->m_value > entry2->m_value ? -1 : 1;
}

void quick_select(hashmap_entry** entries, int k, int start, int end)
{
    recursive_start:
    hashmap_entry* pivot = entries[start];
    int left = start;
    int right = end;

    while (left < right)
    {
        while (left < right && compare_entries(&entries[right], &pivot) <= 0)
        {
            right--;
        }

        entries[left] = entries[right];

        while (left < right && compare_entries(&entries[left], &pivot) >= 0) 
        {
            left++;
        }

        entries[right] = entries[left];
    }

    entries[left] = pivot;

    const int pivot_index = left;
    
    int left_size = pivot_index - start;

    if (k == left_size + 1)
    {
        return;
    }

    if (k <= left_size)
    {
        end = pivot_index - 1;
        goto recursive_start;
    }

    k = k - left_size - 1;
    start = pivot_index + 1;
    goto recursive_start;
}

void process_fingerprint(char* name)
{
    fingerprint* allocated = allocate_back(&article_fingerprint_list);
    allocated->m_id = name;
    
    quick_select(
        global_hashmap.m_entryies, 
        MIN(global_hashmap.m_entry_count, vector_length), 
        /*start:*/ 0, 
        /*end:*/   global_hashmap.m_entry_count - 1);

    qsort(
        global_hashmap.m_entryies,
        MIN(global_hashmap.m_entry_count, vector_length),
        sizeof(hashmap_entry*),
        compare_entries
    );

    int fingerprint_values[128] = { 0 };
    for (int i = 0; i < MIN(global_hashmap.m_entry_count, vector_length); i++) 
    {
        const int weight = global_hashmap.m_entryies[i]->m_value;
        for (int j = 0; j < fingerprint_length; j++)
        {
            fingerprint_values[j] += hashvalue_multiplier[i][j] * weight;
        }
    }

    for (int i = 0; i < fingerprint_length; i++)
    {
        allocated->m_value[i] = (fingerprint_values[i] > 0);
    }
}

static inline void process_articles()
{
    const char* input = "./article.txt";
    FILE* file = fopen(input, "r");
    char buffer[200];
    size_t buffer_used = 0;

    int id_1, id_2;

    while (fscanf(file, "%s", buffer) != EOF)
    {
        char* name = name_allocator.allocate(&name_allocator, strlen(buffer) + 1, sizeof(char));
        strcpy(name, buffer);

        int read_value;
        while ((read_value = fgetc(file)) != EOF)
        {
            if (read_value == '\f')
            {
                break;
            }

            if (!is_alpha(read_value))
            {
                if (buffer_used == 0)
                {
                    continue;
                }

                buffer[buffer_used] = '\0';
                buffer_used++;

                // it is a stopword
                if (is_in_trie(&stopword_trie, buffer))
                {
                    buffer_used = 0;
                    continue;
                }

                // flush
                char* string_space = words_allocator.allocate(
                    &words_allocator,
                    sizeof(char) * buffer_used,
                    sizeof(char)
                );

                strcpy(string_space, buffer);
                put_or_update(&global_hashmap, string_space, 1, hashmap_update_func, buffer_used);

                buffer_used = 0;
                continue;
            }

            buffer[buffer_used] = to_lower(read_value);
            buffer_used++;
        }

        process_fingerprint(name);
        reset_hashmap_and_allocator(&global_hashmap);
    }
}

static inline void get_hashvalue()
{
    const char* input = "./hashvalue.txt";
    FILE* file = fopen(input, "r");

    char buffer[129];

    for (int i = 0; i < vector_length; i++)
    {
        fscanf(file, "%s", buffer);
        for (int j = 0; j < fingerprint_length; j++)
        {
            hashvalue_multiplier[i][j] = buffer[j] == '1' ? 1 : -1;
        }
    }

    fclose(file);
}

int program_main(int argc, char** argv)
{
    vector_length = atoi(argv[1]);
    fingerprint_length = atoi(argv[2]);

    get_hashvalue();
    get_stopwords();
    process_articles();
}
