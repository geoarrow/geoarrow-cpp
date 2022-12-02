#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <gtest/gtest.h>

#include "geoarrow.h"
#include "nanoarrow.h"

class WKTTestException : public std::exception {
 public:
  WKTTestException(const char* step, int code, const char* msg) {
    std::stringstream ss;
    ss << step << "(" << code << "): " << msg;
    message = ss.str();
  }

  const char* what() const noexcept { return message.c_str(); }

 private:
  std::string message;
};

class WKBTester {
 public:
  WKBTester() {
    GeoArrowWKTReaderInit(&reader_);
    GeoArrowWKBWriterInit(&writer_);
    GeoArrowWKBWriterInitVisitor(&writer_, &v_);
    v_.error = &error_;
    array_.release = nullptr;
    ArrowArrayViewInit(&array_view_, NANOARROW_TYPE_BINARY);
  }

  ~WKBTester() {
    GeoArrowWKTReaderReset(&reader_);
    GeoArrowWKBWriterReset(&writer_);
    if (array_.release != nullptr) {
      array_.release(&array_);
    }
    ArrowArrayViewReset(&array_view_);
  }

  std::string LastErrorMessage() { return std::string(error_.message); }

  std::basic_string<uint8_t> AsWKB(const std::string& str) {
    error_.message[0] = '\0';
    if (array_.release != nullptr) {
      array_.release(&array_);
    }

    struct GeoArrowStringView str_view;
    str_view.data = str.data();
    str_view.n_bytes = str.size();

    int result = GeoArrowWKTReaderVisit(&reader_, str_view, &v_);
    if (result != GEOARROW_OK) {
      throw WKTTestException("GeoArrowWKTReaderVisit", result, error_.message);
    }

    result = GeoArrowWKBWriterFinish(&writer_, &array_, &error_);
    if (result != GEOARROW_OK) {
      throw WKTTestException("GeoArrowWKBWriterFinish", result, error_.message);
    }

    result = ArrowArrayViewSetArray(&array_view_, &array_,
                                    reinterpret_cast<struct ArrowError*>(&error_));
    if (result != GEOARROW_OK) {
      throw WKTTestException("ArrowArrayViewSetArray", result, error_.message);
    }

    struct ArrowBufferView answer = ArrowArrayViewGetBytesUnsafe(&array_view_, 0);
    return std::basic_string<uint8_t>(answer.data.as_uint8, answer.n_bytes);
  }

 private:
  struct GeoArrowWKTReader reader_;
  struct GeoArrowWKBWriter writer_;
  struct GeoArrowVisitor v_;
  struct ArrowArray array_;
  struct ArrowArrayView array_view_;
  struct GeoArrowError error_;
};

TEST(WKBWriterTest, WKBWriterTestBasic) {
  struct GeoArrowWKBWriter writer;
  GeoArrowWKBWriterInit(&writer);
  GeoArrowWKBWriterReset(&writer);
}

TEST(WKBWriterTest, WKBWriterTestOneNull) {
  struct GeoArrowWKBWriter writer;
  struct GeoArrowVisitor v;
  GeoArrowWKBWriterInit(&writer);
  GeoArrowWKBWriterInitVisitor(&writer, &v);

  EXPECT_EQ(v.feat_start(&v), GEOARROW_OK);
  EXPECT_EQ(v.null_feat(&v), GEOARROW_OK);
  EXPECT_EQ(v.feat_end(&v), GEOARROW_OK);

  struct ArrowArray array;
  EXPECT_EQ(GeoArrowWKBWriterFinish(&writer, &array, nullptr), GEOARROW_OK);
  EXPECT_EQ(array.length, 1);
  EXPECT_EQ(array.null_count, 1);

  struct ArrowArrayView view;
  ArrowArrayViewInit(&view, NANOARROW_TYPE_STRING);
  ArrowArrayViewSetArray(&view, &array, nullptr);

  EXPECT_TRUE(ArrowArrayViewIsNull(&view, 0));

  ArrowArrayViewReset(&view);
  array.release(&array);
  GeoArrowWKBWriterReset(&writer);
}

TEST(WKBWriterTest, WKBWriterTestOneValidOneNull) {
  struct GeoArrowWKBWriter writer;
  struct GeoArrowVisitor v;
  GeoArrowWKBWriterInit(&writer);
  GeoArrowWKBWriterInitVisitor(&writer, &v);

  EXPECT_EQ(v.feat_start(&v), GEOARROW_OK);
  EXPECT_EQ(v.geom_start(&v, GEOARROW_GEOMETRY_TYPE_POINT, GEOARROW_DIMENSIONS_XY),
            GEOARROW_OK);
  EXPECT_EQ(v.geom_end(&v), GEOARROW_OK);
  EXPECT_EQ(v.feat_end(&v), GEOARROW_OK);

  EXPECT_EQ(v.feat_start(&v), GEOARROW_OK);
  EXPECT_EQ(v.null_feat(&v), GEOARROW_OK);
  EXPECT_EQ(v.feat_end(&v), GEOARROW_OK);

  struct ArrowArray array;
  EXPECT_EQ(GeoArrowWKBWriterFinish(&writer, &array, nullptr), GEOARROW_OK);
  EXPECT_EQ(array.length, 2);
  EXPECT_EQ(array.null_count, 1);

  struct ArrowArrayView view;
  ArrowArrayViewInit(&view, NANOARROW_TYPE_BINARY);
  ASSERT_EQ(ArrowArrayViewSetArray(&view, &array, nullptr), GEOARROW_OK);

  EXPECT_FALSE(ArrowArrayViewIsNull(&view, 0));
  EXPECT_TRUE(ArrowArrayViewIsNull(&view, 1));
  struct ArrowBufferView value = ArrowArrayViewGetBytesUnsafe(&view, 0);

  ArrowArrayViewReset(&view);
  array.release(&array);
  GeoArrowWKBWriterReset(&writer);
}

TEST(WKBWriterTest, WKBWriterTestErrors) {
  struct GeoArrowWKBWriter writer;
  struct GeoArrowVisitor v;
  GeoArrowWKBWriterInit(&writer);
  GeoArrowWKBWriterInitVisitor(&writer, &v);

  // Invalid because level < 0
  EXPECT_EQ(v.feat_start(&v), GEOARROW_OK);
  EXPECT_EQ(v.ring_end(&v), EINVAL);
  EXPECT_EQ(v.coords(&v, nullptr, 0, 2), GEOARROW_OK);

  GeoArrowWKBWriterReset(&writer);
  GeoArrowWKBWriterInit(&writer);
  GeoArrowWKBWriterInitVisitor(&writer, &v);

  // Invalid because of too much nesting
  EXPECT_EQ(v.feat_start(&v), GEOARROW_OK);
  for (int i = 0; i < 32; i++) {
    EXPECT_EQ(v.geom_start(&v, GEOARROW_GEOMETRY_TYPE_POINT, GEOARROW_DIMENSIONS_XY),
              GEOARROW_OK);
  }
  // FIXME!!!
  EXPECT_EQ(v.geom_start(&v, GEOARROW_GEOMETRY_TYPE_POINT, GEOARROW_DIMENSIONS_XY),
            GEOARROW_OK);

  GeoArrowWKBWriterReset(&writer);
}

TEST(WKBWriterTest, WKBWriterTestPoint) {
  WKBTester tester;

  EXPECT_EQ(tester.AsWKB("POINT (30 10)"),
            std::basic_string<uint8_t>({0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x3e, 0x40, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x40}));
}

TEST(WKBWriterTest, WKBWriterTestLinestring) {
  WKBTester tester;

  EXPECT_EQ(tester.AsWKB("LINESTRING (30 10, 12 42)"),
            std::basic_string<uint8_t>(
                {0x01, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x3e, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x24, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x40,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0x40}));
}

TEST(WKBWriterTest, WKBWriterTestPolygon) {
  WKBTester tester;

  EXPECT_EQ(
      tester.AsWKB(
          "POLYGON ((35 10, 45 45, 15 40, 10 20, 35 10), (20 30, 35 35, 30 20, 20 30))"),
      std::basic_string<uint8_t>(
          {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
           0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x41, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
           0x00, 0x24, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x46, 0x40, 0x00, 0x00,
           0x00, 0x00, 0x00, 0x80, 0x46, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2e,
           0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x40, 0x00, 0x00, 0x00, 0x00,
           0x00, 0x00, 0x24, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x40, 0x00,
           0x00, 0x00, 0x00, 0x00, 0x80, 0x41, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
           0x24, 0x40, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34,
           0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x40, 0x00, 0x00, 0x00, 0x00,
           0x00, 0x80, 0x41, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x41, 0x40, 0x00,
           0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
           0x34, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x40, 0x00, 0x00, 0x00,
           0x00, 0x00, 0x00, 0x3e, 0x40}));
}

TEST(WKBReaderTest, WKBReaderTestRoundtripTestingFiles) {
  const char* testing_dir = getenv("GEOARROW_TESTING_DIR");
  if (testing_dir == nullptr || strlen(testing_dir) == 0) {
    GTEST_SKIP();
  }

  WKBTester tester;
  int n_tested = 0;
  // Needs to be bigger than all the WKB items
  uint8_t read_buffer[8096];

  for (const auto& item : std::filesystem::directory_iterator(testing_dir)) {
    // Make sure we have a .wkt file
    std::string path = item.path();
    if (path.size() < 4) {
      continue;
    }

    if (path.substr(path.size() - 4, path.size()) != ".wkt") {
      continue;
    }

    std::stringstream wkb_path_builder;
    wkb_path_builder << path.substr(0, path.size() - 4) << ".wkb";

    // Expect that all lines roundtrip
    std::ifstream infile(path);
    std::ifstream infile_wkb(wkb_path_builder.str());
    std::string line;
    while (std::getline(infile, line)) {
      std::basic_string<uint8_t> actual = tester.AsWKB(line);
      if (actual.size() > sizeof(read_buffer)) {
        throw std::runtime_error("Read buffer for testing was too small");
      }

      infile_wkb.read((char*)read_buffer, actual.size());
      std::cout << path << "[" << n_tested << "]"
                << "\n";
      ASSERT_EQ(actual, std::basic_string<uint8_t>(read_buffer, actual.size()));
    }

    n_tested++;
  }

  // Make sure at least one file was tested
  EXPECT_GT(n_tested, 0);
}
