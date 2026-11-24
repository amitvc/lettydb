#include <gtest/gtest.h>
#include <cstring>
#include "storage/tuple.h"
#include "catalog/schema.h"
#include "catalog/column.h"
#include "catalog/catalog_defs.h"

namespace letty {

class TupleTest : public ::testing::Test {
 protected:
  static Schema make_mixed_schema() {
    return Schema({
      Column("id",     DataType::INTEGER, 4,  0),
      Column("name",   DataType::VARCHAR, 64, 4),
      Column("active", DataType::BOOLEAN, 1,  68),
    });
  }

  static Schema make_numeric_schema() {
    return Schema({
      Column("id",    DataType::INTEGER, 4, 0),
      Column("score", DataType::DOUBLE,  8, 4),
    });
  }

  static Schema make_date_schema() {
    return Schema({
      Column("created", DataType::DATE, 10, 0),
    });
  }

  static Schema make_timestamp_schema() {
    return Schema({
      Column("ts", DataType::TIMESTAMP, 19, 0),
    });
  }
};

TEST_F(TupleTest, GetValue_ReturnsCorrectValues) {
  Tuple t(std::vector<Value>{int32_t{99}, bool{false}});
  EXPECT_EQ(std::get<int32_t>(t.get_value(0)), 99);
  EXPECT_EQ(std::get<bool>(t.get_value(1)), false);
}

TEST_F(TupleTest, SerializeInto_Integer_WritesCorrectBytes) {
  Schema schema({Column("n", DataType::INTEGER, 4, 0)});
  Tuple t(std::vector<Value>{int32_t{400}});

  Byte buf[16] = {};
  uint32_t out_size = t.serialize(schema, buf, sizeof(buf));
  EXPECT_EQ(out_size, 5u);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0u);

  int32_t val;
  std::memcpy(&val, buf + 1, sizeof(int32_t));
  EXPECT_EQ(val, 400);
}

TEST_F(TupleTest, SerializeInto_NegativeInteger_WritesCorrectBytes) {
  Schema schema({Column("n", DataType::INTEGER, 4, 0)});
  Tuple t(std::vector<Value>{int32_t{-1}});

  Byte buf[16] = {};
  uint32_t out_size = t.serialize(schema, buf, sizeof(buf));

  int32_t val;
  std::memcpy(&val, buf + 1, sizeof(int32_t));
  EXPECT_EQ(val, -1);
}

TEST_F(TupleTest, SerializeInto_Double_WritesCorrectBytes) {
  Schema schema({Column("x", DataType::DOUBLE, 8, 0)});
  const double expected = 3.14159;
  Tuple t(std::vector<Value>{expected});

  Byte buf[16] = {};
  uint32_t out_size = t.serialize(schema, buf, sizeof(buf));
  EXPECT_EQ(out_size, 9u);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0u);

  double val;
  std::memcpy(&val, buf + 1, sizeof(double));
  EXPECT_DOUBLE_EQ(val, expected);
}

TEST_F(TupleTest, SerializeInto_BooleanTrue_WritesOneByte1) {
  Schema schema({Column("flag", DataType::BOOLEAN, 1, 0)});
  Tuple t(std::vector<Value>{bool{true}});

  Byte buf[8] = {};
  uint32_t out_size = t.serialize(schema, buf, sizeof(buf));
  EXPECT_EQ(out_size, 2u);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0u);
  EXPECT_EQ(static_cast<uint8_t>(buf[1]), 1u);
}

TEST_F(TupleTest, SerializeInto_BooleanFalse_WritesOneByte0) {
  Schema schema({Column("flag", DataType::BOOLEAN, 1, 0)});
  Tuple t(std::vector<Value>{bool{false}});

  Byte buf[8] = {};
  uint32_t out_size = t.serialize(schema, buf, sizeof(buf));
  EXPECT_EQ(out_size, 2u);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0u);
  EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0u);
}

TEST_F(TupleTest, SerializeInto_MultipleFixedColumns_TotalSizeIsSum) {
  Schema schema = make_numeric_schema();
  Tuple t(std::vector<Value>{int32_t{1}, double{2.0}});

  Byte buf[64] = {};
  uint32_t out_size = t.serialize(schema, buf, sizeof(buf));
  EXPECT_EQ(out_size, 13u);
}


TEST_F(TupleTest, SerializeInto_Varchar_Writes2ByteLengthPrefixThenData) {
  Schema schema({Column("name", DataType::VARCHAR, 64, 0)});
  const std::string name = "alice";
  Tuple t(std::vector<Value>{name});

  Byte buf[64] = {};
  uint32_t out_size = t.serialize(schema, buf, sizeof(buf));
  EXPECT_EQ(out_size, 3u + name.size());
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0u);

  uint16_t len;
  std::memcpy(&len, buf + 1, sizeof(uint16_t));
  EXPECT_EQ(len, static_cast<uint16_t>(name.size()));
  EXPECT_EQ(std::memcmp(buf + 3, name.data(), name.size()), 0);
}

TEST_F(TupleTest, SerializeInto_EmptyVarchar_WritesTwoZeroBytes) {
  Schema schema({Column("name", DataType::VARCHAR, 64, 0)});
  Tuple t(std::vector<Value>{std::string{""}});

  Byte buf[16] = {};
  uint32_t out_size = t.serialize(schema, buf, sizeof(buf));
  EXPECT_EQ(out_size, 3u);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0u);

  uint16_t len;
  std::memcpy(&len, buf + 1, sizeof(uint16_t));
  EXPECT_EQ(len, 0u);
}

TEST_F(TupleTest, SerializeInto_NumericSchema_WritesCorrectContent) {
  Schema schema = make_numeric_schema();
  Tuple t(std::vector<Value>{int32_t{7}, double{1.5}});

  Byte buf[64] = {};
  uint32_t out_size = t.serialize(schema, buf, sizeof(buf));
  EXPECT_EQ(out_size, 13u);

  int32_t id;
  double score;
  std::memcpy(&id, buf + 1, sizeof(int32_t));
  std::memcpy(&score, buf + 5, sizeof(double));
  EXPECT_EQ(id, 7);
  EXPECT_DOUBLE_EQ(score, 1.5);
}


TEST_F(TupleTest, SerializeInto_BufferTooSmall_Throws) {
  Schema schema = make_numeric_schema();
  Tuple t(std::vector<Value>{int32_t{1}, double{2.0}});

  Byte buf[4] = {};
  EXPECT_THROW(t.serialize(schema, buf, sizeof(buf)), std::runtime_error);
}

TEST_F(TupleTest, SerializeInto_ValueCountMismatch_Throws) {
  Schema schema = make_numeric_schema();
  Tuple t(std::vector<Value>{int32_t{1}});

  Byte buf[64] = {};
  EXPECT_THROW(t.serialize(schema, buf, sizeof(buf)), std::runtime_error);
}

TEST_F(TupleTest, Deserialize_Integer_ReturnsCorrectValue) {
  Schema schema({Column("n", DataType::INTEGER, 4, 0)});
  int32_t raw = 42;
  Byte buf[5] = {};
  std::memcpy(buf + 1, &raw, sizeof(raw));
  Tuple t = Tuple::deserialize(schema, buf, sizeof(buf));
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(std::get<int32_t>(t.get_value(0)), 42);
}

TEST_F(TupleTest, Deserialize_Double_ReturnsCorrectValue) {
  Schema schema({Column("x", DataType::DOUBLE, 8, 0)});
  double raw = 2.718;
  Byte buf[9] = {};
  std::memcpy(buf + 1, &raw, sizeof(raw));
  Tuple t = Tuple::deserialize(schema, buf, sizeof(buf));
  ASSERT_EQ(t.size(), 1u);
  EXPECT_DOUBLE_EQ(std::get<double>(t.get_value(0)), 2.718);
}

TEST_F(TupleTest, Deserialize_BooleanTrue_ReturnsTrueValue) {
  Schema schema({Column("b", DataType::BOOLEAN, 1, 0)});
  Byte raw[2] = {0, 1};
  Tuple t = Tuple::deserialize(schema, raw, sizeof(raw));
  ASSERT_EQ(t.size(), 1u);
  EXPECT_TRUE(std::get<bool>(t.get_value(0)));
}

TEST_F(TupleTest, Deserialize_BooleanNonZero_ReturnsTrueValue) {
  Schema schema({Column("b", DataType::BOOLEAN, 1, 0)});
  Byte raw[2] = {0, static_cast<Byte>(0xFF)};
  Tuple t = Tuple::deserialize(schema, raw, sizeof(raw));
  EXPECT_TRUE(std::get<bool>(t.get_value(0)));
}

TEST_F(TupleTest, Deserialize_BooleanFalse_ReturnsFalseValue) {
  Schema schema({Column("b", DataType::BOOLEAN, 1, 0)});
  Byte raw[2] = {0, 0};
  Tuple t = Tuple::deserialize(schema, raw, sizeof(raw));
  EXPECT_FALSE(std::get<bool>(t.get_value(0)));
}

TEST_F(TupleTest, Deserialize_Varchar_ReturnsCorrectString) {
  Schema schema({Column("s", DataType::VARCHAR, 64, 0)});
  const char* word = "hello";
  uint16_t len = 5;
  Byte buf[16] = {};
  std::memcpy(buf + 1, &len, sizeof(uint16_t));
  std::memcpy(buf + 3, word, 5);

  Tuple t = Tuple::deserialize(schema, buf, sizeof(buf));
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(std::get<std::string>(t.get_value(0)), "hello");
}

TEST_F(TupleTest, Deserialize_Date_TrimsNullPadding) {
  Schema schema = make_date_schema();
  Byte buf[11] = {};
  const char* date = "2026-2-19";
  std::memcpy(buf + 1, date, 9);

  Tuple t = Tuple::deserialize(schema, buf, sizeof(buf));
  ASSERT_EQ(t.size(), 1u);
  const auto& val = std::get<std::string>(t.get_value(0));
  EXPECT_EQ(val.find('\0'), std::string::npos) << "null byte should have been trimmed";
  EXPECT_EQ(val, "2026-2-19");
}

TEST_F(TupleTest, RoundTrip_NumericSchema_PreservesAllValues) {
  Schema schema = make_numeric_schema();
  Tuple original(std::vector<Value>{int32_t{-1}, double{99.9}});

  Byte buf[64] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 2u);
  EXPECT_EQ(std::get<int32_t>(restored.get_value(0)), -1);
  EXPECT_DOUBLE_EQ(std::get<double>(restored.get_value(1)), 99.9);
}

TEST_F(TupleTest, RoundTrip_NullValue_PreservesNull) {
  Schema schema = make_numeric_schema();
  Tuple original(std::vector<Value>{int32_t{7}, std::monostate{}});

  Byte buf[64] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0b00000010);

  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 2u);
  EXPECT_EQ(std::get<int32_t>(restored.get_value(0)), 7);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(restored.get_value(1)));
}

TEST_F(TupleTest, RoundTrip_MixedSchema_PreservesAllValues) {
  Schema schema = make_mixed_schema();
  Tuple original(std::vector<Value>{int32_t{42}, std::string{"alice"}, bool{true}});

  Byte buf[256] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 3u);
  EXPECT_EQ(std::get<int32_t>(restored.get_value(0)), 42);
  EXPECT_EQ(std::get<std::string>(restored.get_value(1)), "alice");
  EXPECT_EQ(std::get<bool>(restored.get_value(2)), true);
}

TEST_F(TupleTest, RoundTrip_EmptyVarchar_PreservesEmptyString) {
  Schema schema({Column("s", DataType::VARCHAR, 64, 0)});
  Tuple original(std::vector<Value>{std::string{""}});

  Byte buf[64] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 1u);
  EXPECT_EQ(std::get<std::string>(restored.get_value(0)), "");
}

TEST_F(TupleTest, RoundTrip_LargeVarchar_PreservesAllCharacters) {
  Schema schema({Column("s", DataType::VARCHAR, 1024, 0)});
  std::string long_str(500, 'z');
  Tuple original(std::vector<Value>{long_str});

  Byte buf[512] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 1u);
  EXPECT_EQ(std::get<std::string>(restored.get_value(0)), long_str);
}

TEST_F(TupleTest, RoundTrip_DateField_PreservesValue) {
  Schema schema = make_date_schema();
  const std::string date = "2026-02-19";
  Tuple original(std::vector<Value>{date});

  Byte buf[64] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 1u);
  EXPECT_EQ(std::get<std::string>(restored.get_value(0)), date);
}

TEST_F(TupleTest, RoundTrip_TimestampField_PreservesValue) {
  Schema schema = make_timestamp_schema();
  const std::string ts = "2026-02-19 10:30:00";
  Tuple original(std::vector<Value>{ts});

  Byte buf[64] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 1u);
  EXPECT_EQ(std::get<std::string>(restored.get_value(0)), ts);
}

TEST_F(TupleTest, RoundTrip_TinyIntField_PreservesValue) {
  Schema schema({Column("status", DataType::TINYINT, 1, 0)});
  int8_t val = 42;
  Tuple original(std::vector<Value>{val});

  Byte buf[64] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 1u);
  EXPECT_EQ(std::get<int8_t>(restored.get_value(0)), val);
}

TEST_F(TupleTest, RoundTrip_SmallIntField_PreservesValue) {
  Schema schema({Column("val", DataType::SMALLINT, 2, 0)});
  int16_t val = 12345;
  Tuple original(std::vector<Value>{val});

  Byte buf[64] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 1u);
  EXPECT_EQ(std::get<int16_t>(restored.get_value(0)), val);
}

TEST_F(TupleTest, RoundTrip_BigIntField_PreservesValue) {
  Schema schema({Column("big", DataType::BIGINT, 8, 0)});
  int64_t val = 987654321012345LL;
  Tuple original(std::vector<Value>{val});

  Byte buf[64] = {};
  uint32_t out_size = original.serialize(schema, buf, sizeof(buf));
  Tuple restored = Tuple::deserialize(schema, buf, out_size);

  ASSERT_EQ(restored.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(restored.get_value(0)), val);
}

}  // namespace letty
