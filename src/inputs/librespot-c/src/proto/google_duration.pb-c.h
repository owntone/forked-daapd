/* Generated by the protocol buffer compiler.  DO NOT EDIT! */
/* Generated from: google_duration.proto */

#ifndef PROTOBUF_C_google_5fduration_2eproto__INCLUDED
#define PROTOBUF_C_google_5fduration_2eproto__INCLUDED

#include <protobuf-c/protobuf-c.h>

PROTOBUF_C__BEGIN_DECLS

#if PROTOBUF_C_VERSION_NUMBER < 1003000
# error This file was generated by a newer version of protoc-c which is incompatible with your libprotobuf-c headers. Please update your headers.
#elif 1004001 < PROTOBUF_C_MIN_COMPILER_VERSION
# error This file was generated by an older version of protoc-c which is incompatible with your libprotobuf-c headers. Please regenerate this file with a newer version of protoc-c.
#endif


typedef struct Google__Protobuf__Duration Google__Protobuf__Duration;


/* --- enums --- */


/* --- messages --- */

struct  Google__Protobuf__Duration
{
  ProtobufCMessage base;
  int64_t seconds;
  int32_t nanos;
};
#define GOOGLE__PROTOBUF__DURATION__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&google__protobuf__duration__descriptor) \
    , 0, 0 }


/* Google__Protobuf__Duration methods */
void   google__protobuf__duration__init
                     (Google__Protobuf__Duration         *message);
size_t google__protobuf__duration__get_packed_size
                     (const Google__Protobuf__Duration   *message);
size_t google__protobuf__duration__pack
                     (const Google__Protobuf__Duration   *message,
                      uint8_t             *out);
size_t google__protobuf__duration__pack_to_buffer
                     (const Google__Protobuf__Duration   *message,
                      ProtobufCBuffer     *buffer);
Google__Protobuf__Duration *
       google__protobuf__duration__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   google__protobuf__duration__free_unpacked
                     (Google__Protobuf__Duration *message,
                      ProtobufCAllocator *allocator);
/* --- per-message closures --- */

typedef void (*Google__Protobuf__Duration_Closure)
                 (const Google__Protobuf__Duration *message,
                  void *closure_data);

/* --- services --- */


/* --- descriptors --- */

extern const ProtobufCMessageDescriptor google__protobuf__duration__descriptor;

PROTOBUF_C__END_DECLS


#endif  /* PROTOBUF_C_google_5fduration_2eproto__INCLUDED */
