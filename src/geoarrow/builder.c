
#include <string.h>

#include "nanoarrow.h"

#include "geoarrow.h"

struct BuilderPrivate {
  // The ArrowArray responsible for owning the memory
  struct ArrowArray array;

  // Cached pointers pointing inside the array's private data
  // Depending on what exactly is being built, these pointers
  // might be NULL.
  struct ArrowBitmap* validity;
  struct ArrowBuffer* buffers[8];

  // To avoid one additional layer of private_data, these are fields
  // needed by various builder types.

  // Fields to keep track of states when using the visitor pattern
  // 32 is the maximum recursion depth for nested collections (could
  // almost certainly be much lower).
  enum GeoArrowGeometryType geometry_type[32];
  enum GeoArrowDimensions dimensions[32];
  int64_t size_pos[32];
  uint32_t size[32];
  int32_t level;

  // Options
  int significant_digits;
  int use_flat_multipoint;
};

static GeoArrowErrorCode GeoArrowBuilderInitInternal(struct GeoArrowBuilder* builder) {
  enum GeoArrowType type = builder->view.schema_view.type;

  // Initialize an array view to help set some fields
  struct GeoArrowArrayView array_view;
  NANOARROW_RETURN_NOT_OK(GeoArrowArrayViewInitFromType(&array_view, type));

  struct BuilderPrivate* private =
      (struct BuilderPrivate*)ArrowMalloc(sizeof(struct BuilderPrivate));
  if (private == NULL) {
    return ENOMEM;
  }

  memset(private, 0, sizeof(struct BuilderPrivate));

  struct ArrowSchema schema;
  int result = GeoArrowSchemaInit(&schema, type);
  if (result != GEOARROW_OK) {
    ArrowFree(private);
    return result;
  }

  result = ArrowArrayInitFromSchema(&private->array, &schema, NULL);
  schema.release(&schema);
  if (result != GEOARROW_OK) {
    ArrowFree(private);
    return result;
  }

  // Update a few things about the writable view from the regular view
  builder->view.coords.n_values = array_view.coords.n_values;
  builder->view.coords.coords_stride = array_view.coords.coords_stride;
  switch (builder->view.schema_view.coord_type) {
    case GEOARROW_COORD_TYPE_SEPARATE:
      builder->view.n_buffers = 1 + array_view.n_offsets + array_view.coords.n_values;
      break;

    // interleaved + WKB + WKT
    default:
      builder->view.n_buffers = 1 + array_view.n_offsets + 1;
      break;
  }

  // Cache the ArrowBitmap and ArrowBuffer pointers we need to allocate
  private->validity = ArrowArrayValidityBitmap(&private->array);

  struct _GeoArrowFindBufferResult res;
  for (int64_t i = 0; i < builder->view.n_buffers; i++) {
    res.array = NULL;
    _GeoArrowArrayFindBuffer(&private->array, &res, i, 0, 0);
    if (res.array == NULL) {
      ArrowFree(private);
      return EINVAL;
    }

    private->buffers[i] = ArrowArrayBuffer(res.array, res.i);
  }

  // Some default options
  private->significant_digits = 16;
  private->use_flat_multipoint = 1;

  builder->private_data = private;
  return GEOARROW_OK;
}

GeoArrowErrorCode GeoArrowBuilderInitFromType(struct GeoArrowBuilder* builder,
                                              enum GeoArrowType type) {
  memset(builder, 0, sizeof(struct GeoArrowBuilder));
  NANOARROW_RETURN_NOT_OK(
      GeoArrowSchemaViewInitFromType(&builder->view.schema_view, type));
  return GeoArrowBuilderInitInternal(builder);
}

GeoArrowErrorCode GeoArrowBuilderInitFromSchema(struct GeoArrowBuilder* builder,
                                                struct ArrowSchema* schema,
                                                struct GeoArrowError* error) {
  memset(builder, 0, sizeof(struct GeoArrowBuilder));
  NANOARROW_RETURN_NOT_OK(
      GeoArrowSchemaViewInit(&builder->view.schema_view, schema, error));
  return GeoArrowBuilderInitInternal(builder);
}

GeoArrowErrorCode GeoArrowBuilderReserveBuffer(struct GeoArrowBuilder* builder, int64_t i,
                                               int64_t additional_size_bytes) {
  struct BuilderPrivate* private = (struct BuilderPrivate*)builder->private_data;
  struct ArrowBuffer* buffer_src = private->buffers[i];
  struct GeoArrowWritableBufferView* buffer_dst = builder->view.buffers + i;

  // Sync any changes from the builder's view of the buffer to nanoarrow's
  buffer_src->size_bytes = buffer_dst->size_bytes;

  // Use nanoarrow's reserve
  NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buffer_src, additional_size_bytes));

  // Sync any changes back to the builder's view
  builder->view.buffers[i].data.data = buffer_src->data;
  builder->view.buffers[i].capacity_bytes = buffer_src->capacity_bytes;
  return GEOARROW_OK;
}

void GeoArrowBuilderInitVisitor(struct GeoArrowBuilder* builder,
                                struct GeoArrowVisitor* v) {
  return;
}

static void GeoArrowSetArrayLengthFromBufferLength(struct GeoArrowSchemaView* schema_view,
                                                   struct _GeoArrowFindBufferResult* res,
                                                   int64_t size_bytes);

static void GeoArrowSetCoordContainerLength(struct GeoArrowBuilder* builder);

GeoArrowErrorCode GeoArrowBuilderFinish(struct GeoArrowBuilder* builder,
                                        struct ArrowArray* array,
                                        struct GeoArrowError* error) {
  struct BuilderPrivate* private = (struct BuilderPrivate*)builder->private_data;

  // Sync builder buffer's back to the array; set array lengths from buffer sizes
  struct _GeoArrowFindBufferResult res;
  for (int64_t i = 0; i < builder->view.n_buffers; i++) {
    private->buffers[i]->size_bytes = builder->view.buffers[i].size_bytes;

    res.array = NULL;
    _GeoArrowArrayFindBuffer(&private->array, &res, i, 0, 0);
    if (res.array == NULL) {
      return EINVAL;
    }

    GeoArrowSetArrayLengthFromBufferLength(&builder->view.schema_view, &res,
                                           private->buffers[i]->size_bytes);
  }

  // Set the struct or fixed-size list container length
  GeoArrowSetCoordContainerLength(builder);

  // Call finish building, which will flush the buffer pointers into the array
  // and validate sizes.
  NANOARROW_RETURN_NOT_OK(
      ArrowArrayFinishBuilding(&private->array, (struct ArrowError*)error));

  // Move the result
  memcpy(array, &private->array, sizeof(struct ArrowArray));
  private->array.release = NULL;

  return GEOARROW_OK;
}

void GeoArrowBuilderReset(struct GeoArrowBuilder* builder) {
  if (builder->private_data != NULL) {
    struct BuilderPrivate* private = (struct BuilderPrivate*)builder->private_data;
    if (private->array.release != NULL) {
      private->array.release(&private->array);
    }

    ArrowFree(private);
    builder->private_data = NULL;
  }
}

static void GeoArrowSetArrayLengthFromBufferLength(struct GeoArrowSchemaView* schema_view,
                                                   struct _GeoArrowFindBufferResult* res,
                                                   int64_t size_bytes) {
  // By luck, buffer index 1 for every array is the one we use to infer the length;
  // however, this is a slightly different formula for each type/depth
  if (res->i != 1) {
    return;
  }

  switch (schema_view->type) {
    case GEOARROW_TYPE_WKB:
      res->array->length = (size_bytes / sizeof(int32_t)) - 1;
      return;
    case GEOARROW_TYPE_LARGE_WKB:
      res->array->length = (size_bytes / sizeof(int64_t)) - 1;
      return;
    default:
      break;
  }

  int coord_level;
  switch (schema_view->geometry_type) {
    case GEOARROW_GEOMETRY_TYPE_POINT:
      coord_level = 0;
      break;
    case GEOARROW_GEOMETRY_TYPE_LINESTRING:
    case GEOARROW_GEOMETRY_TYPE_MULTIPOINT:
      coord_level = 1;
      break;
    case GEOARROW_GEOMETRY_TYPE_POLYGON:
    case GEOARROW_GEOMETRY_TYPE_MULTILINESTRING:
      coord_level = 2;
      break;
    case GEOARROW_GEOMETRY_TYPE_MULTIPOLYGON:
      coord_level = 3;
      break;
    default:
      return;
  }

  if (res->level < coord_level) {
    // This is an offset buffer
    res->array->length = (size_bytes / sizeof(int32_t)) - 1;
  } else {
    // This is a data buffer
    res->array->length = size_bytes / sizeof(double);
  }
}

static void GeoArrowSetCoordContainerLength(struct GeoArrowBuilder* builder) {
  struct BuilderPrivate* private = (struct BuilderPrivate*)builder->private_data;

  // At this point all the array lengths should be set except for the
  // fixed-size list or struct parent to the coordinate array(s).
  int scale;
  switch (builder->view.schema_view.coord_type) {
    case GEOARROW_COORD_TYPE_SEPARATE:
      scale = 1;
      break;
    case GEOARROW_COORD_TYPE_INTERLEAVED:
      switch (builder->view.schema_view.dimensions) {
        case GEOARROW_DIMENSIONS_XY:
          scale = 2;
          break;
        case GEOARROW_DIMENSIONS_XYZ:
        case GEOARROW_DIMENSIONS_XYM:
          scale = 3;
          break;
        case GEOARROW_DIMENSIONS_XYZM:
          scale = 4;
          break;
        default:
          return;
      }
      break;
    default:
      // e.g., WKB
      break;
  }

  switch (builder->view.schema_view.geometry_type) {
    case GEOARROW_GEOMETRY_TYPE_POINT:
      private
      ->array.length = private->array.children[0]->length;
      break;
    case GEOARROW_GEOMETRY_TYPE_LINESTRING:
    case GEOARROW_GEOMETRY_TYPE_MULTIPOINT:
      private
      ->array.children[0]->length = private->array.children[0]->children[0]->length;
      break;
    case GEOARROW_GEOMETRY_TYPE_POLYGON:
    case GEOARROW_GEOMETRY_TYPE_MULTILINESTRING:
      private
      ->array.children[0]->children[0]->length =
          private->array.children[0]->children[0]->children[0]->length;
      break;
    case GEOARROW_GEOMETRY_TYPE_MULTIPOLYGON:
      private
      ->array.children[0]->children[0]->children[0]->length =
          private->array.children[0]->children[0]->children[0]->children[0]->length;
      break;
    default:
      // e.g., WKB
      break;
  }
}

static int feat_start_point(struct GeoArrowVisitor* v) {
  // Reset level, reset feature coordinate count
  return GEOARROW_OK;
}

static int null_feat_point(struct GeoArrowVisitor* v) {
  // Write an empty to coordinates, write a null to buffer 1
  return GEOARROW_OK;
}

static int geom_start_point(struct GeoArrowVisitor* v,
                            enum GeoArrowGeometryType geometry_type,
                            enum GeoArrowDimensions dimensions) {
  // level++, geometry type, dimensions, reset size
  // validate dimensions, maybe against some options that indicate
  // error for mismatch, fill, or drop behaviour
  return GEOARROW_OK;
}

static int ring_start_point(struct GeoArrowVisitor* v) {
  // level++ geometry type, dimensions, reset size
  return GEOARROW_OK;
}

static int coords_point(struct GeoArrowVisitor* v,
                        const struct GeoArrowCoordView* coords) {
  // write coords to coords
  // solve how exactly to map a conversion from one to another if the
  // dims don't align
  return GEOARROW_OK;
}

static int ring_end_point(struct GeoArrowVisitor* v) {
  // level--
  return GEOARROW_OK;
}

static int geom_end_point(struct GeoArrowVisitor* v) {
  // level--
  return GEOARROW_OK;
}

static int feat_end_point(struct GeoArrowVisitor* v) {
  // if there weren't any coords (i.e., EMPTY), we need to write some NANs here
  // if there was >1 coords, we also need to error or we'll get misaligned output
  return GEOARROW_OK;
}

static void GeoArrowVisitorInitPoint(struct GeoArrowBuilder* builder,
                                     struct GeoArrowVisitor* v) {
  struct GeoArrowError* previous_error = v->error;
  GeoArrowVisitorInitVoid(v);
  v->error = previous_error;

  v->feat_start = &feat_start_point;
  v->null_feat = &null_feat_point;
  v->geom_start = &geom_start_point;
  v->ring_start = &ring_start_point;
  v->coords = &coords_point;
  v->ring_end = &ring_end_point;
  v->geom_end = &geom_end_point;
  v->feat_end = &feat_end_point;
  v->private_data = builder->private_data;
}
