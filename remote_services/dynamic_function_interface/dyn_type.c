/**
 * Licensed under Apache License v2. See LICENSE for more information.
 */
#include "dyn_type.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <errno.h>

#include "dyn_common.h"

DFI_SETUP_LOG(dynType)

static int dynType_parseWithStream(FILE *stream, const char *name, dyn_type *parent, struct reference_types_head *refTypes, dyn_type **result);
static void dynType_clear(dyn_type *type);
static void dynType_clearComplex(dyn_type *type);
static void dynType_clearSequence(dyn_type *type);
static void dynType_clearTypedPointer(dyn_type *type);

static ffi_type * dynType_ffiTypeFor(int c);
static dyn_type * dynType_findType(dyn_type *type, char *name);
static int dynType_parseAny(FILE *stream, dyn_type *type);
static int dynType_parseComplex(FILE *stream, dyn_type *type);
static int dynType_parseNestedType(FILE *stream, dyn_type *type);
static int dynType_parseReference(FILE *stream, dyn_type *type);
static int dynType_parseRefByValue(FILE *stream, dyn_type *type);
static int dynType_parseSequence(FILE *stream, dyn_type *type);
static int dynType_parseSimple(int c, dyn_type *type);
static int dynType_parseTypedPointer(FILE *stream, dyn_type *type);
static void dynType_prepCif(ffi_type *type);
static unsigned short dynType_getOffset(dyn_type *type, int index);

static void dynType_printAny(char *name, dyn_type *type, int depth, FILE *stream);
static void dynType_printComplex(char *name, dyn_type *type, int depth, FILE *stream);
static void dynType_printSequence(char *name, dyn_type *type, int depth, FILE *stream);
static void dynType_printSimple(char *name, dyn_type *type, int depth, FILE *stream);
static void dynType_printTypedPointer(char *name, dyn_type *type, int depth, FILE *stream);
static void dynType_printDepth(int depth, FILE *stream);

static void dynType_printTypes(dyn_type *type, FILE *stream);
static void dynType_printComplexType(dyn_type *type, FILE *stream);
static void dynType_printSimpleType(dyn_type *type, FILE *stream);

static int dynType_parseText(FILE *stream, dyn_type *type);

struct generic_sequence {
    uint32_t cap;
    uint32_t len;
    void *buf;
};

static const int OK = 0;
static const int ERROR = 1;
static const int MEM_ERROR = 2;
static const int PARSE_ERROR = 3;

int dynType_parse(FILE *descriptorStream, const char *name, struct reference_types_head *refTypes, dyn_type **type) {
    return dynType_parseWithStream(descriptorStream, name, NULL, refTypes, type);
}

int dynType_parseWithStr(const char *descriptor, const char *name, struct reference_types_head *refTypes, dyn_type **type) {
    int status = OK;
    FILE *stream = fmemopen((char *)descriptor, strlen(descriptor), "r");
    if (stream != NULL) {
        status = dynType_parseWithStream(stream, name, NULL, refTypes, type);
        if (status == OK) {
            int c = fgetc(stream);
            if (c != '\0' && c != EOF) {
                status = PARSE_ERROR;
                LOG_ERROR("Expected EOF got %c", c);
            }
        } 
        fclose(stream);
    } else {
        status = ERROR;
        LOG_ERROR("Error creating mem stream for descriptor string. %s", strerror(errno)); 
    }
    return status;
}

static int dynType_parseWithStream(FILE *stream, const char *name, dyn_type *parent, struct reference_types_head *refTypes, dyn_type **result) {
    int status = OK;
    dyn_type *type = calloc(1, sizeof(*type));
    if (type != NULL) {
        type->parent = parent;
        type->type = DYN_TYPE_INVALID;
        type->referenceTypes = refTypes;
        TAILQ_INIT(&type->nestedTypesHead);
        if (name != NULL) {
            type->name = strdup(name);
            if (type->name == NULL) {
                status = MEM_ERROR;
                LOG_ERROR("Error strdup'ing name '%s'\n", name);			
            } 
        }
        if (status == OK) {
            status = dynType_parseAny(stream, type);        
        }
        if (status == OK) {
            *result = type;
        } else {
            dynType_destroy(type);
        }
    } else {
        status = MEM_ERROR;
        LOG_ERROR("Error allocating memory for type");
    }
    return status;
}

static int dynType_parseAny(FILE *stream, dyn_type *type) {
    int status = OK;

    int c = fgetc(stream);
    switch(c) {
        case 'T' :
            status = dynType_parseNestedType(stream, type);
            if (status == OK) {
                status = dynType_parseAny(stream, type);
            } 
            break;
        case 'L' :
            status = dynType_parseReference(stream, type);
            break;
        case 'l' :
            status = dynType_parseRefByValue(stream, type);
            break;
        case '{' :
            status = dynType_parseComplex(stream, type);
            break;
        case '[' :
            status = dynType_parseSequence(stream, type);
            break;
        case '*' :
            status = dynType_parseTypedPointer(stream, type);
            break;
        case 't' :
            status = dynType_parseText(stream, type);
            break;
        default :
            status = dynType_parseSimple(c, type);
            break;
    }

    return status;
}

static int dynType_parseText(FILE *stream, dyn_type *type) {
    int status = OK;
    type->type = DYN_TYPE_TEXT;
    type->descriptor = 't';
    type->ffiType = &ffi_type_pointer;
    return status;
}

static int dynType_parseComplex(FILE *stream, dyn_type *type) {
    int status = OK;
    type->type = DYN_TYPE_COMPLEX;
    type->descriptor = '{';
    type->ffiType = &type->complex.structType;
    TAILQ_INIT(&type->complex.entriesHead);

    int c = fgetc(stream);
    struct complex_type_entry *entry = NULL;
    while (c != ' ' && c != '}') {
        ungetc(c,stream);
        entry = calloc(1, sizeof(*entry));
        if (entry != NULL) {
            entry->type.parent = type;
            entry->type.type = DYN_TYPE_INVALID;
            TAILQ_INIT(&entry->type.nestedTypesHead);
            TAILQ_INSERT_TAIL(&type->complex.entriesHead, entry, entries);
            status = dynType_parseAny(stream, &entry->type);
        } else {
            status = MEM_ERROR;
            LOG_ERROR("Error allocating memory for type");
        }
        c = fgetc(stream);
    }

    entry = TAILQ_FIRST(&type->complex.entriesHead);
    char *name = NULL;
    while (c == ' ' && entry != NULL) {
        status = dynCommon_parseName(stream, &name);
        if (status == OK) {
            entry->name = name;
            entry = TAILQ_NEXT(entry, entries);
        } else {
            break;
        }
        c = getc(stream); 
    }

    int count = 0;
    TAILQ_FOREACH(entry, &type->complex.entriesHead, entries) {
        count +=1;
    }

    if (status == OK) {
        type->complex.structType.type =  FFI_TYPE_STRUCT;
        type->complex.structType.elements = calloc(count + 1, sizeof(ffi_type));
        type->complex.structType.elements[count] = NULL;
        if (type->complex.structType.elements != NULL) {
            int index = 0;
            TAILQ_FOREACH(entry, &type->complex.entriesHead, entries) {
                type->complex.structType.elements[index++] = entry->type.ffiType;
            }
        } else {
            status = MEM_ERROR;
            //T\nODO log: error allocating memory
        }
    }

    if (status == OK) {
        type->complex.types = calloc(count, sizeof(dyn_type *));
        if (type != NULL) {
            int index = 0;
            TAILQ_FOREACH(entry, &type->complex.entriesHead, entries) {
                type->complex.types[index++] = &entry->type;
            }
        } else {
            status = MEM_ERROR;
            LOG_ERROR("Error allocating memory for type")
        }
    }

    if (status == OK) {
        dynType_prepCif(type->ffiType);
    }


    return status;
}

static int dynType_parseNestedType(FILE *stream, dyn_type *type) {
    int status = OK;
    char *name = NULL;
    struct nested_entry *entry = NULL; 

    entry = calloc(1, sizeof(*entry));
    if (entry != NULL) {
        entry->type.parent = type;
        entry->type.type = DYN_TYPE_INVALID;
        TAILQ_INIT(&entry->type.nestedTypesHead);
        TAILQ_INSERT_TAIL(&type->nestedTypesHead, entry, entries);
        status = dynCommon_parseName(stream, &name);
        entry->type.name = name;
    } else {
        status = MEM_ERROR;
    }     

    if (status == OK) {
        int c = fgetc(stream);
        if (c != '=') {
            status = PARSE_ERROR;
            LOG_ERROR("Error parsing nested type expected '=' got '%c'", c);
        }
    }

    if (status == OK) {
        status = dynType_parseAny(stream, &entry->type);
        int c = fgetc(stream);
        if (c != ';') {
            status = PARSE_ERROR;
            LOG_ERROR("Expected ';' got '%c'\n", c);
        }
    }

    return status;
}

static int dynType_parseReference(FILE *stream, dyn_type *type) {
    int status = OK;
    type->type = DYN_TYPE_TYPED_POINTER;
    type->descriptor = '*';

    type->ffiType = &ffi_type_pointer;
    type->typedPointer.typedType =  NULL;

    dyn_type *subType = calloc(1, sizeof(*subType));

    if (subType != NULL) {
        type->typedPointer.typedType = subType;
        subType->parent = type;
        subType->type = DYN_TYPE_INVALID;
        TAILQ_INIT(&subType->nestedTypesHead);
        status = dynType_parseRefByValue(stream, subType);
    } else {
        status = MEM_ERROR;
        LOG_ERROR("Error allocating memory for subtype\n");
    }

    return status;
}

static int dynType_parseRefByValue(FILE *stream, dyn_type *type) {
    int status = OK;
    type->type = DYN_TYPE_REF;
    type->descriptor = 'l';

    char *name = NULL;
    status = dynCommon_parseName(stream, &name);
    if (status == OK) {
        dyn_type *ref = dynType_findType(type, name);
        if (ref != NULL) {
            type->ref.ref = ref;
        } else {
            status = PARSE_ERROR;
            LOG_ERROR("Error cannot find type '%s'", name);
        }
        free(name);
    } 

    if (status == OK) {
        int c = fgetc(stream);
        if (c != ';') {
            status = PARSE_ERROR;
            LOG_ERROR("Error expected ';' got '%c'", c);
        } 
    }

    return status;
}

static ffi_type *seq_types[] = {&ffi_type_uint32, &ffi_type_uint32, &ffi_type_pointer, NULL};

static int dynType_parseSequence(FILE *stream, dyn_type *type) {
    int status = OK;
    type->type = DYN_TYPE_SEQUENCE;
    type->descriptor = '[';

    type->sequence.seqType.elements = seq_types;
    status = dynType_parseWithStream(stream, NULL, type, NULL, &type->sequence.itemType);

    if (status == OK) {
        type->ffiType = &type->sequence.seqType;
        dynType_prepCif(&type->sequence.seqType);
    }

    return status;
}

static int dynType_parseSimple(int c, dyn_type *type) {
    int status = OK;
    ffi_type *ffiType = dynType_ffiTypeFor(c);
    if (ffiType != NULL) {
        type->type = DYN_TYPE_SIMPLE;
        type->descriptor = c;
        type->ffiType = ffiType;
    } else {
        status = PARSE_ERROR;
        LOG_ERROR("Error unsupported type '%c'", c);
    }

    return status;
}

static int dynType_parseTypedPointer(FILE *stream, dyn_type *type) {
    int status = OK;
    type->type = DYN_TYPE_TYPED_POINTER;
    type->descriptor = '*';
    type->ffiType = &ffi_type_pointer;

    status = dynType_parseWithStream(stream, NULL, type, NULL, &type->typedPointer.typedType);

    return status;
}

static void dynType_prepCif(ffi_type *type) {
    ffi_cif cif;
    ffi_type *args[1];
    args[0] = type;
    ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_uint, args);
}

void dynType_destroy(dyn_type *type) {
    if (type != NULL) {          
        dynType_clear(type);
        free(type);
    }
}

static void dynType_clear(dyn_type *type) {
    struct nested_entry *entry = TAILQ_FIRST(&type->nestedTypesHead);
    struct nested_entry *tmp = NULL;
    while (entry != NULL) {
        tmp = entry;
        entry = TAILQ_NEXT(entry, entries);
        dynType_clear(&tmp->type);
        free(tmp);
    }

    switch (type->type) {
        case DYN_TYPE_COMPLEX :
            dynType_clearComplex(type);
            break;
        case DYN_TYPE_SEQUENCE :
            dynType_clearSequence(type);
            break;
        case DYN_TYPE_TYPED_POINTER :
            dynType_clearTypedPointer(type);
            break;
    } 

    if (type->name != NULL) {
        free(type->name);
    }
}

static void dynType_clearComplex(dyn_type *type) {
    assert(type->type == DYN_TYPE_COMPLEX);
    struct complex_type_entry *entry = TAILQ_FIRST(&type->complex.entriesHead);
    struct complex_type_entry *tmp = NULL;
    while (entry != NULL) {
        dynType_clear(&entry->type);
        if (entry->name != NULL) {
            free(entry->name);
        }
        tmp = entry;
        entry = TAILQ_NEXT(entry, entries);
        free(tmp);
    }
    if (type->complex.types != NULL) {
        free(type->complex.types);
    }
    if (type->complex.structType.elements != NULL) {
        free(type->complex.structType.elements);
    }
}

static void dynType_clearSequence(dyn_type *type) {
    assert(type->type == DYN_TYPE_SEQUENCE);
    if (type->sequence.itemType != NULL) {
        dynType_destroy(type->sequence.itemType);
    }
}

static void dynType_clearTypedPointer(dyn_type *type) {
    assert(type->type == DYN_TYPE_TYPED_POINTER);
    if (type->typedPointer.typedType != NULL) {
        dynType_destroy(type->typedPointer.typedType);
    }
}

int dynType_alloc(dyn_type *type, void **bufLoc) {
    assert(type->type != DYN_TYPE_REF);
    int status = OK;

    void *inst = calloc(1, type->ffiType->size);
    if (inst != NULL) {
        *bufLoc = inst;
    } else {
        status = MEM_ERROR;
        LOG_ERROR("Error allocating memory for type '%c'", type->descriptor);
    }

    return status;
}


int dynType_complex_indexForName(dyn_type *type, const char *name) {
    assert(type->type == DYN_TYPE_COMPLEX);
    int i = 0;
    int index = -1;
    struct complex_type_entry *entry = NULL;
    TAILQ_FOREACH(entry, &type->complex.entriesHead, entries) {
        if (strcmp(name, entry->name) == 0) {
            index = i;
        }
        i +=1;
    }
    return index;
}

int dynType_complex_dynTypeAt(dyn_type *type, int index, dyn_type **result) {
    assert(type->type == DYN_TYPE_COMPLEX);
    dyn_type *sub = type->complex.types[index];
    if (sub->type == DYN_TYPE_REF) {
        sub = sub->ref.ref;
    }
    *result = sub;
    return 0;
}

int dynType_complex_setValueAt(dyn_type *type, int index, void *start, void *in) {
    assert(type->type == DYN_TYPE_COMPLEX);
    char *loc = ((char *)start) + dynType_getOffset(type, index);
    size_t size = type->complex.structType.elements[index]->size;
    memcpy(loc, in, size);
    return 0;
}

int dynType_complex_valLocAt(dyn_type *type, int index, void *inst, void **result) {
    assert(type->type == DYN_TYPE_COMPLEX);
    char *l = (char *)inst;
    void *loc = (void *)(l + dynType_getOffset(type, index));
    *result = loc;
    return 0;
}

int dynType_complex_entries(dyn_type *type, struct complex_type_entries_head **entries) {
    assert(type->type == DYN_TYPE_COMPLEX);
    int status = OK;
    *entries = &type->complex.entriesHead;
    return status;
}

//sequence
int dynType_sequence_alloc(dyn_type *type, void *inst, int cap, void **out) {
    assert(type->type == DYN_TYPE_SEQUENCE);
    int status = OK;
    struct generic_sequence *seq = inst;
    if (seq != NULL) {
        size_t size = dynType_size(type->sequence.itemType);
        seq->buf = calloc(cap, size);
        if (seq->buf != NULL) {
            seq->cap = cap;
            seq->len = 0;
            *out = seq->buf;
        } else {
            seq->cap = 0;
            status = MEM_ERROR;
            LOG_ERROR("Error allocating memory for buf")
        }
    } else {
            status = MEM_ERROR;
            LOG_ERROR("Error allocating memory for seq")
    }
    return status;
}

void dynType_free(dyn_type *type, void *loc) {
    //TODO
    LOG_INFO("TODO dynType_free");
}

uint32_t dynType_sequence_length(void *seqLoc) {
    struct generic_sequence *seq = seqLoc;
    return seq->len;
}

int dynType_sequence_locForIndex(dyn_type *type, void *seqLoc, int index, void **out) {
    assert(type->type == DYN_TYPE_SEQUENCE);
    int status = OK;

    struct generic_sequence *seq = seqLoc;
    char *valLoc = seq->buf;
    size_t itemSize = type->sequence.itemType->ffiType->size;

    if (index >= seq->cap) {
        status = ERROR;
        LOG_ERROR("Requested index (%i) is greater than capacity (%u) of sequence", index, seq->cap);
    }

    if (index >= seq->len) {
        LOG_WARNING("Requesting index (%i) outsize defined length (%u) but within capacity", index, seq->len);
    }

    if (status == OK) { }
    int i;
    for (i = 0; i < seq->cap; i += 1) {
        if (index == i) {
            break;
        } else {
            valLoc += itemSize;
        }
    }

    (*out) = valLoc;

    return status;
}

int dynType_sequence_increaseLengthAndReturnLastLoc(dyn_type *type, void *seqLoc, void **valLoc) {
    assert(type->type == DYN_TYPE_SEQUENCE);
    int status = OK;
    struct generic_sequence *seq = seqLoc;

    int lastIndex = seq->len;
    if (seq->len < seq->cap) {
        seq->len += 1;
    } else {
        status = ERROR;
        LOG_ERROR("Cannot increase sequence length beyond capacity (%u)", seq->cap);
    }

    if (status == OK) {
        status = dynType_sequence_locForIndex(type, seqLoc, lastIndex, valLoc);
    }

    return status;
}

dyn_type * dynType_sequence_itemType(dyn_type *type) {
    assert(type->type == DYN_TYPE_SEQUENCE);
    dyn_type *itemType = type->sequence.itemType;
    if (itemType->type == DYN_TYPE_REF) {
        itemType = itemType->ref.ref;
    }
    return itemType;
}

void dynType_simple_setValue(dyn_type *type, void *inst, void *in) {
    size_t size = dynType_size(type);
    memcpy(inst, in, size);
}


int dynType_descriptorType(dyn_type *type) {
    return type->descriptor;
}

static ffi_type * dynType_ffiTypeFor(int c) {
    ffi_type *type = NULL;
    switch (c) {
        case 'F' :
            type = &ffi_type_float;
            break;
        case 'D' :
            type = &ffi_type_double;
            break;
        case 'B' :
            type = &ffi_type_sint8;
            break;
        case 'b' :
            type = &ffi_type_uint8;
            break;
        case 'S' :
            type = &ffi_type_sint16;
            break;
        case 's' :
            type = &ffi_type_uint16;
            break;
        case 'I' :
            type = &ffi_type_sint32;
            break;
        case 'i' :
            type = &ffi_type_uint32;
            break;
        case 'J' :
            type = &ffi_type_sint64;
            break;
        case 'j' :
            type = &ffi_type_sint64;
            break;
        case 'N' :
            type = &ffi_type_sint;
            break;
        case 'P' :
            type = &ffi_type_pointer;
            break;
    }
    return type;
}

static dyn_type * dynType_findType(dyn_type *type, char *name) {
    dyn_type *result = NULL;

    struct type_entry *entry = NULL;
    if (type->referenceTypes != NULL) {
        TAILQ_FOREACH(entry, type->referenceTypes, entries) {
            LOG_DEBUG("checking ref type '%s' with name '%s'", entry->type->name, name);
            if (strcmp(name, entry->type->name) == 0) {
                result = entry->type;
                break;
            }
        }
    }

    if (result == NULL) {
        struct nested_entry *nEntry = NULL;
        TAILQ_FOREACH(nEntry, &type->nestedTypesHead, entries) {
            LOG_DEBUG("checking nested type '%s' with name '%s'", nEntry->type.name, name);
            if (strcmp(name, nEntry->type.name) == 0) {
                result = &nEntry->type;
                break;
            }
        }
    }

    if (result == NULL && type->parent != NULL) {
        result = dynType_findType(type->parent, name);
    }

    return result;
}

static unsigned short dynType_getOffset(dyn_type *type, int index) {
    assert(type->type == DYN_TYPE_COMPLEX);
    unsigned short offset = 0;

    ffi_type *ffiType = &type->complex.structType;
    int i;
    for (i = 0;  i <= index && ffiType->elements[i] != NULL; i += 1) {
        size_t size = ffiType->elements[i]->size;
        unsigned short alignment = ffiType->elements[i]->alignment;
        int alignment_diff = offset % alignment;
        if (alignment_diff > 0) {
            offset += (alignment - alignment_diff);
        }
        if (i < index) {
            offset += size;
        }
    }

    return offset;
}

size_t dynType_size(dyn_type *type) {
    return type->ffiType->size;
}

int dynType_type(dyn_type *type) {
    return type->type;
}


int dynType_typedPointer_getTypedType(dyn_type *type, dyn_type **out) {
    assert(type->type == DYN_TYPE_TYPED_POINTER);
    int status = 0;

    dyn_type *typedType = type->typedPointer.typedType;
    if (typedType->type == DYN_TYPE_REF) {
        typedType = typedType->ref.ref;
    }

    *out = typedType;
    return status;
}


int dynType_text_allocAndInit(dyn_type *type, void *textLoc, const char *value) {
    int status = 0;
    const char *str = strdup(value);
    char const **loc = textLoc;
    if (str != NULL) {
        *loc = str;
    } else {
        status = ERROR;
        LOG_ERROR("Cannot allocate memory for string");
    }
    return status;
}















void dynType_print(dyn_type *type, FILE *stream) {
    if (type != NULL) {
        dynType_printTypes(type, stream);

        fprintf(stream, "main type:\n");
        dynType_printAny("root", type, 0, stream);
    } else {
        fprintf(stream, "invalid type\n");
    }
}

static void dynType_printDepth(int depth, FILE *stream) {
    int i;
    for (i = 0; i < depth; i +=1 ) {
        fprintf(stream, "\t");
    }
}

static void dynType_printAny(char *name, dyn_type *type, int depth, FILE *stream) {
    dyn_type *toPrint = type;
    if (toPrint->type == DYN_TYPE_REF) {
        toPrint = toPrint->ref.ref;
    }
    switch(toPrint->type) {
        case DYN_TYPE_COMPLEX :
            dynType_printComplex(name, toPrint, depth, stream);
            break;
        case DYN_TYPE_SIMPLE :
            dynType_printSimple(name, toPrint, depth, stream);
            break;
        case DYN_TYPE_SEQUENCE :
            dynType_printSequence(name, toPrint, depth, stream);
            break;
        case DYN_TYPE_TYPED_POINTER :
            dynType_printTypedPointer(name, toPrint, depth, stream);
            break;
        default :
            fprintf(stream, "TODO Unsupported type %i\n", toPrint->type);
            break;
    }
}

static void dynType_printComplex(char *name, dyn_type *type, int depth, FILE *stream) {
    if (type->name == NULL) {
        dynType_printDepth(depth, stream);
        fprintf(stream, "%s: complex type (anon), size is %zu, alignment is %i, descriptor is '%c'. fields:\n", name,  type->ffiType->size, type->ffiType->alignment, type->descriptor);

        struct complex_type_entry *entry = NULL;
        TAILQ_FOREACH(entry, &type->complex.entriesHead, entries) {
            dynType_printAny(entry->name, &entry->type, depth + 1, stream);
        }

        dynType_printDepth(depth, stream);
        printf("}\n");
    } else {
        dynType_printDepth(depth, stream);
        fprintf(stream, "%s: complex type ('%s'), size is %zu, alignment is %i, descriptor is '%c'.\n", name, type->name, type->ffiType->size, type->ffiType->alignment, type->descriptor);
    }
}

static void dynType_printSequence(char *name, dyn_type *type, int depth, FILE *stream) {
    dynType_printDepth(depth, stream);
    fprintf(stream, "sequence, size is %zu, alignment is %i, descriptor is '%c'. fields:\n", type->ffiType->size, type->ffiType->alignment, type->descriptor);

    dynType_printDepth(depth + 1, stream);
    fprintf(stream, "cap: simple type, size is %zu, alignment is %i.\n", type->sequence.seqType.elements[0]->size, type->sequence.seqType.elements[0]->alignment);

    dynType_printDepth(depth + 1, stream);
    fprintf(stream, "len: simple type, size is %zu, alignment is %i.\n", type->sequence.seqType.elements[1]->size, type->sequence.seqType.elements[1]->alignment);

    dynType_printDepth(depth + 1, stream);
    fprintf(stream, "buf: array, size is %zu, alignment is %i. points to ->\n", type->sequence.seqType.elements[2]->size, type->sequence.seqType.elements[2]->alignment);
    dynType_printAny("element", type->sequence.itemType, depth + 1, stream);
}

static void dynType_printSimple(char *name, dyn_type *type, int depth, FILE *stream) {
    dynType_printDepth(depth, stream);
    fprintf(stream, "%s: simple type, size is %zu, alignment is %i, descriptor is '%c'.\n", name, type->ffiType->size, type->ffiType->alignment, type->descriptor);
}

static void dynType_printTypedPointer(char *name, dyn_type *type, int depth, FILE *stream) {
    dynType_printDepth(depth, stream);
    fprintf(stream, "%s: typed pointer, size is %zu, alignment is %i, points to ->\n", name, type->ffiType->size, type->ffiType->alignment);
    char *subName = NULL;
    if (name != NULL) {
        char buf[128];
        snprintf(buf, 128, "*%s", name);
        subName = buf;
    }
    dynType_printAny(subName, type->typedPointer.typedType, depth + 1, stream);
}

static void dynType_printTypes(dyn_type *type, FILE *stream) {

    dyn_type *parent = type->parent;
    struct nested_entry *pentry = NULL;
    while (parent != NULL) {
        TAILQ_FOREACH(pentry, &parent->nestedTypesHead, entries) {
            if (&pentry->type == type) {
                return;
            }
        }
        parent = parent->parent;
    }

    struct nested_entry *entry = NULL;
    TAILQ_FOREACH(entry, &type->nestedTypesHead, entries) {
        dyn_type *toPrint = &entry->type;
        if (toPrint->type == DYN_TYPE_REF) {
            toPrint = toPrint->ref.ref;
        }

        switch(toPrint->type) {
            case DYN_TYPE_COMPLEX :
                dynType_printComplexType(toPrint, stream);
                break;
            case DYN_TYPE_SIMPLE :
                dynType_printSimpleType(toPrint, stream);
                break;
            default :
                printf("TODO Print Type\n");
                break;
        }
    }


    struct complex_type_entry *centry = NULL;
    switch(type->type) {
        case DYN_TYPE_COMPLEX :
            TAILQ_FOREACH(centry, &type->complex.entriesHead, entries) {
                dynType_printTypes(&centry->type, stream);
            }
            break;
        case DYN_TYPE_SEQUENCE :
            dynType_printTypes(type->sequence.itemType, stream);
            break;
        case DYN_TYPE_TYPED_POINTER :
            dynType_printTypes(type->typedPointer.typedType, stream);
            break;
    }
}

static void dynType_printComplexType(dyn_type *type, FILE *stream) {
    fprintf(stream, "type '%s': complex type, size is %zu, alignment is %i, descriptor is '%c'. fields:\n", type->name,  type->ffiType->size, type->ffiType->alignment, type->descriptor);

    struct complex_type_entry *entry = NULL;
    TAILQ_FOREACH(entry, &type->complex.entriesHead, entries) {
        dynType_printAny(entry->name, &entry->type, 2, stream);
    }

    fprintf(stream, "}\n");
}

static void dynType_printSimpleType(dyn_type *type, FILE *stream) {
    fprintf(stream, "\ttype '%s': simple type, size is %zu, alignment is %i, descriptor is '%c'\n", type->name, type->ffiType->size, type->ffiType->alignment, type->descriptor);
}

