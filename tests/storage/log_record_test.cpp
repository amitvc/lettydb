#include "storage/log_record.h"

#include "storage/page_utils.h"

#include <gtest/gtest.h>

namespace letty {

  TEST(LogRecordTest, SerializePagePatch) {
    PagePatch original{};
    original.page_id = 20;
    original.offset = 100;
    original.before_payload = {0x01, 0x02, 0x03};
    original.after_payload = {0xAA, 0xBB};

    std::vector<Byte> bytes;
    ASSERT_TRUE(serialize_page_patch(original, bytes));

    PagePatch decoded{};
    ASSERT_TRUE(deserialize_page_patch(bytes, decoded));

    EXPECT_EQ(decoded.page_id, original.page_id);
    EXPECT_EQ(decoded.offset, original.offset);
    EXPECT_EQ(decoded.before_payload, original.before_payload);
    EXPECT_EQ(decoded.after_payload, original.after_payload);
  }

  TEST(LogRecordTest, DeserializePagePatchRejectsSizeMismatchWithoutMutatingOutput) {
    PagePatch original{};
    original.page_id = 20;
    original.offset = 10;
    original.before_payload = {0x01, 0x02};
    original.after_payload = {0x03};

    std::vector<Byte> bytes;
    ASSERT_TRUE(serialize_page_patch(original, bytes));

    // Corrupt before_payloadsize (byte offset 6) to claim more bytes than the buffer actually has.
    write_le16(bytes.data() + 6, 9999);

    PagePatch out{};
    out.page_id = 777; // sentinel - must not change on failure
    out.offset = 555;

    EXPECT_FALSE(deserialize_page_patch(bytes, out));
    EXPECT_EQ(out.page_id, 777);
    EXPECT_EQ(out.offset, 555);
  }

  TEST(LogRecordTest, PagePatchRoundTripsWithEmptyBeforePayload) {
    PagePatch payload{};
    payload.page_id = 7;
    payload.offset = 50;
    payload.before_payload = {};  // e.g. a brand-new page with no prior content
    payload.after_payload = {0xDE, 0xAD, 0xBE, 0xEF};

    std::vector<Byte> bytes;
    ASSERT_TRUE(serialize_page_patch(payload, bytes));

    PagePatch decoded{};
    ASSERT_TRUE(deserialize_page_patch(bytes, decoded));

    EXPECT_TRUE(decoded.before_payload.empty());
    EXPECT_EQ(decoded.after_payload, payload.after_payload);
  }

  TEST(LogRecordTest, PagePatchRoundTripsWithEmptyAfterPayload) {
    PagePatch payload{};
    payload.page_id = 8;
    payload.offset = 50;
    payload.before_payload = {0xCA, 0xFE};
    payload.after_payload = {};

    std::vector<Byte> bytes;
    ASSERT_TRUE(serialize_page_patch(payload, bytes));

    PagePatch decoded{};
    ASSERT_TRUE(deserialize_page_patch(bytes, decoded));

    EXPECT_EQ(decoded.before_payload, payload.before_payload);
    EXPECT_TRUE(decoded.after_payload.empty());
  }

  TEST(LogRecordTest, SerializePagePatchFailsOnInvalidPageID) {
    PagePatch payload{};
    payload.page_id = INVALID_PAGE_ID;
    payload.offset = 50;
    payload.before_payload = {0xCA, 0xFE};
    payload.after_payload = {};

    std::vector<Byte> bytes;
    ASSERT_FALSE(serialize_page_patch(payload, bytes));
  }

  TEST(LogRecordTest, SerializeCompensationPatchRoundTrips) {
    CompensationPatch original{};
    original.page_id = 30;
    original.offset = 200;
    original.undo_next_lsn = 123456;
    original.payload = {0x10, 0x20, 0x30, 0x40};

    std::vector<Byte> bytes;
    ASSERT_TRUE(serialize_compensation_patch(original, bytes));

    CompensationPatch decoded{};
    ASSERT_TRUE(deserialize_compensation_patch(bytes, decoded));

    EXPECT_EQ(decoded.page_id, original.page_id);
    EXPECT_EQ(decoded.offset, original.offset);
    EXPECT_EQ(decoded.undo_next_lsn, original.undo_next_lsn);
    EXPECT_EQ(decoded.payload, original.payload);
  }

  TEST(LogRecordTest, SerializeCompensationPatchFailsOnInvalidPageID) {
    CompensationPatch payload{};
    payload.page_id = INVALID_PAGE_ID;
    payload.offset = 10;
    payload.undo_next_lsn = 1;
    payload.payload = {0x01};

    std::vector<Byte> bytes;
    EXPECT_FALSE(serialize_compensation_patch(payload, bytes));
  }

  TEST(LogRecordTest, DeserializeCompensationPatchRejectsSizeMismatchWithoutMutatingOutput) {
    CompensationPatch original{};
    original.page_id = 30;
    original.offset = 10;
    original.undo_next_lsn = 55;
    original.payload = {0x01, 0x02};

    std::vector<Byte> bytes;
    ASSERT_TRUE(serialize_compensation_patch(original, bytes));

    // Corrupt payload_size (byte offset 14: page_id 4 + offset 2 + undo_next_lsn 8) to claim
    // more bytes than the buffer actually has.
    write_le16(bytes.data() + 14, 9999);

    CompensationPatch out{};
    out.page_id = 777; // sentinel - must not change on failure
    out.undo_next_lsn = 888;

    EXPECT_FALSE(deserialize_compensation_patch(bytes, out));
    EXPECT_EQ(out.page_id, 777);
    EXPECT_EQ(out.undo_next_lsn, 888);
  }

  TEST(LogRecordTest, CompensationPatchRoundTripsWithEmptyPayload) {
    CompensationPatch payload{};
    payload.page_id = 9;
    payload.offset = 20;
    payload.undo_next_lsn = 99;
    payload.payload = {};

    std::vector<Byte> bytes;
    ASSERT_TRUE(serialize_compensation_patch(payload, bytes));

    CompensationPatch decoded{};
    ASSERT_TRUE(deserialize_compensation_patch(bytes, decoded));
    EXPECT_TRUE(decoded.payload.empty());
  }

  TEST(LogRecordTest, SerializeLogRecordRoundTripsPageUpdate) {
    PagePatch patch{};
    patch.page_id = 42;
    patch.offset = 128;
    patch.before_payload = {0x11, 0x22, 0x33};
    patch.after_payload = {0x44, 0x55, 0x66, 0x77};

    LogRecord original{};
    ASSERT_TRUE(build_page_patch_record(7, patch, original));
    original.header.lsn = 1024;
    original.header.prev_lsn = 512;

    ByteBuffer bytes;
    ASSERT_TRUE(serialize_log_record(original, bytes));
    EXPECT_EQ(bytes.size(), sizeof(LogRecordHeader) + original.payload.size());

    LogRecord decoded{};
    ASSERT_TRUE(deserialize_log_record(bytes, decoded));

    EXPECT_EQ(decoded.header.type, LogRecordType::PAGE_UPDATE);
    EXPECT_EQ(decoded.header.lsn, 1024u);
    EXPECT_EQ(decoded.header.prev_lsn, 512u);
    EXPECT_EQ(decoded.header.tran_id, 7u);
    EXPECT_EQ(decoded.header.payload_size, original.payload.size());
    EXPECT_NE(decoded.header.checksum, 0u);
    EXPECT_EQ(decoded.payload, original.payload);

    PagePatch decoded_patch{};
    ASSERT_TRUE(deserialize_page_patch(decoded.payload, decoded_patch));
    EXPECT_EQ(decoded_patch.page_id, 42);
    EXPECT_EQ(decoded_patch.offset, 128);
    EXPECT_EQ(decoded_patch.before_payload, patch.before_payload);
    EXPECT_EQ(decoded_patch.after_payload, patch.after_payload);
  }

  TEST(LogRecordTest, SerializeLogRecordRoundTripsEmptyPayloadControlRecord) {
    LogRecord original{};
    original.header.type = LogRecordType::COMMIT;
    original.header.lsn = 2048;
    original.header.prev_lsn = 1024;
    original.header.tran_id = 99;
    original.header.payload_size = 0;
    original.header.checksum = 0;

    ByteBuffer bytes;
    ASSERT_TRUE(serialize_log_record(original, bytes));
    EXPECT_EQ(bytes.size(), sizeof(LogRecordHeader));

    LogRecord decoded{};
    ASSERT_TRUE(deserialize_log_record(bytes, decoded));
    EXPECT_EQ(decoded.header.type, LogRecordType::COMMIT);
    EXPECT_EQ(decoded.header.lsn, 2048u);
    EXPECT_EQ(decoded.header.prev_lsn, 1024u);
    EXPECT_EQ(decoded.header.tran_id, 99u);
    EXPECT_EQ(decoded.header.payload_size, 0u);
    EXPECT_TRUE(decoded.payload.empty());
  }

  TEST(LogRecordTest, DeserializeLogRecordDetectsChecksumCorruption) {
    PagePatch patch{};
    patch.page_id = 12;
    patch.offset = 64;
    patch.before_payload = {0xAA};
    patch.after_payload = {0xBB};

    LogRecord original{};
    ASSERT_TRUE(build_page_patch_record(3, patch, original));
    original.header.lsn = 400;
    original.header.prev_lsn = 200;

    ByteBuffer bytes;
    ASSERT_TRUE(serialize_log_record(original, bytes));

    bytes.back() ^= 0xFF;

    LogRecord out{};
    out.header.tran_id = 777;
    EXPECT_FALSE(deserialize_log_record(bytes, out));
    EXPECT_EQ(out.header.tran_id, 777u);
  }

  TEST(LogRecordTest, DeserializeLogRecordRejectsTruncatedAndSizeMismatchedBuffers) {
    ByteBuffer too_short(sizeof(LogRecordHeader) - 1, 0);
    LogRecord out{};
    EXPECT_FALSE(deserialize_log_record(too_short, out));

    LogRecord original{};
    original.header.type = LogRecordType::BEGIN;
    original.header.lsn = 100;
    original.header.prev_lsn = 0;
    original.header.tran_id = 1;
    original.header.payload_size = 0;

    ByteBuffer bytes;
    ASSERT_TRUE(serialize_log_record(original, bytes));
    bytes.push_back(0x00);

    EXPECT_FALSE(deserialize_log_record(bytes, out));
  }

  TEST(LogRecordTest, SerializeLogRecordFailsWhenHeaderPayloadSizeMismatchesVector) {
    LogRecord bad{};
    bad.header.type = LogRecordType::PAGE_UPDATE;
    bad.header.payload_size = 10;
    bad.payload = {0x01, 0x02};

    ByteBuffer bytes;
    EXPECT_FALSE(serialize_log_record(bad, bytes));
  }

  TEST(LogRecordTest, SerializeGamPatch) {
	GamPatch original{};
	original.gam_page_id = 20;
	original.bit_index = 100;

	std::vector<Byte> bytes;
	ASSERT_TRUE(serialize_extent_patch(original, bytes));

  	GamPatch decoded{};
	ASSERT_TRUE(deserialize_extent_patch(bytes, decoded));

	EXPECT_EQ(decoded.gam_page_id, original.gam_page_id);
	EXPECT_EQ(decoded.bit_index, original.bit_index);
  }

  TEST(LogRecordTest, SerializeIamPatch) {
	IamPatch original{};
	original.iam_page_id = 20;
	original.extent_id = 1024;

	std::vector<Byte> bytes;
	ASSERT_TRUE(serialize_iam_patch(original, bytes));

  	IamPatch decoded{};
	ASSERT_TRUE(deserialize_iam_patch(bytes, decoded));

	EXPECT_EQ(decoded.iam_page_id, original.iam_page_id);
	EXPECT_EQ(decoded.extent_id, original.extent_id);
  }

}
