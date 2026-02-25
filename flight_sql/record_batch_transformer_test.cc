/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include <odbcabstraction/platform.h>
#include <arrow/array/array_binary.h>
#include <arrow/array/array_primitive.h>
#include <arrow/builder.h>
#include "arrow/testing/builder.h"
#include "record_batch_transformer.h"
#include "gtest/gtest.h"
#include <arrow/record_batch.h>
using namespace arrow;

namespace {
std::shared_ptr<RecordBatch> CreateOriginalRecordBatch() {
  std::vector<int> values = {1, 2, 3, 4, 5};
  std::shared_ptr<Array> array;

  ArrayFromVector<Int32Type, int32_t>(values, &array);

  auto schema = arrow::schema({field("test", int32(), false)});

  return RecordBatch::Make(schema, 4, {array});
}
} // namespace

namespace driver {
namespace flight_sql {

TEST(Transformer, TransformerRenameTest) {
  // Prepare the Original Record Batch
  auto original_record_batch = CreateOriginalRecordBatch();
  auto schema = original_record_batch->schema();

  // Execute the transformation of the Record Batch
  std::string original_name("test");
  std::string transformed_name("test1");

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .RenameField(original_name, transformed_name)
                         .Build();

  auto transformed_record_batch = transformer->Transform(original_record_batch);

  auto transformed_array_ptr =
      transformed_record_batch->GetColumnByName(transformed_name);

  auto original_array_ptr =
      original_record_batch->GetColumnByName(original_name);

  // Assert that the arrays are being the same and we are not creating new
  // buffers
  ASSERT_EQ(transformed_array_ptr, original_array_ptr);

  // Assert if the schema is not the same
  ASSERT_NE(original_record_batch->schema(),
            transformed_record_batch->schema());
  // Assert if the data is not changed
  ASSERT_EQ(original_record_batch->GetColumnByName(original_name),
            transformed_record_batch->GetColumnByName(transformed_name));
}

TEST(Transformer, TransformerAddEmptyVectorTest) {
  // Prepare the Original Record Batch
  auto original_record_batch = CreateOriginalRecordBatch();
  auto schema = original_record_batch->schema();

  std::string original_name("test");
  std::string transformed_name("test1");
  auto emptyField = std::string("empty");

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .RenameField(original_name, transformed_name)
                         .AddFieldOfNulls(emptyField, int32())
                         .Build();

  auto transformed_schema = transformer->GetTransformedSchema();

  ASSERT_EQ(transformed_schema->num_fields(), 2);
  ASSERT_EQ(transformed_schema->GetFieldIndex(transformed_name), 0);
  ASSERT_EQ(transformed_schema->GetFieldIndex(emptyField), 1);

  auto transformed_record_batch = transformer->Transform(original_record_batch);

  auto transformed_array_ptr =
      transformed_record_batch->GetColumnByName(transformed_name);

  auto original_array_ptr =
      original_record_batch->GetColumnByName(original_name);

  // Assert that the arrays are being the same and we are not creating new
  // buffers
  ASSERT_EQ(transformed_array_ptr, original_array_ptr);

  // Assert if the schema is not the same
  ASSERT_NE(original_record_batch->schema(),
            transformed_record_batch->schema());
  // Assert if the data is not changed
  ASSERT_EQ(original_record_batch->GetColumnByName(original_name),
            transformed_record_batch->GetColumnByName(transformed_name));
}

TEST(Transformer, TransformerChangingOrderOfArrayTest) {
  std::vector<int> first_array_value = {1, 2, 3, 4, 5};
  std::vector<int> second_array_value = {6, 7, 8, 9, 10};
  std::vector<int> third_array_value = {2, 4, 6, 8, 10};
  std::shared_ptr<Array> first_array;
  std::shared_ptr<Array> second_array;
  std::shared_ptr<Array> third_array;

  ArrayFromVector<Int32Type, int32_t>(first_array_value, &first_array);
  ArrayFromVector<Int32Type, int32_t>(second_array_value, &second_array);
  ArrayFromVector<Int32Type, int32_t>(third_array_value, &third_array);

  auto schema = arrow::schema({field("first_array", int32(), false),
                               field("second_array", int32(), false),
                               field("third_array", int32(), false)});

  auto original_record_batch =
      RecordBatch::Make(schema, 5, {first_array, second_array, third_array});

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .RenameField("third_array", "test3")
                         .RenameField("second_array", "test2")
                         .RenameField("first_array", "test1")
                         .AddFieldOfNulls("empty", int32())
                         .Build();

  const std::shared_ptr<RecordBatch> &transformed_record_batch =
      transformer->Transform(original_record_batch);

  auto transformed_schema = transformed_record_batch->schema();

  // Assert to check if the empty fields was added
  ASSERT_EQ(transformed_record_batch->num_columns(), 4);

  // Assert to make sure that the elements changed his order.
  ASSERT_EQ(transformed_schema->GetFieldIndex("test3"), 0);
  ASSERT_EQ(transformed_schema->GetFieldIndex("test2"), 1);
  ASSERT_EQ(transformed_schema->GetFieldIndex("test1"), 2);
  ASSERT_EQ(transformed_schema->GetFieldIndex("empty"), 3);

  // Assert to make sure that the data didn't change after renaming the arrays
  ASSERT_EQ(transformed_record_batch->GetColumnByName("test3"), third_array);
  ASSERT_EQ(transformed_record_batch->GetColumnByName("test2"), second_array);
  ASSERT_EQ(transformed_record_batch->GetColumnByName("test1"), first_array);
}
TEST(Transformer, TransformerReplaceFieldValuesTest) {
  // Create a RecordBatch with a string column containing "BASE TABLE" and "VIEW"
  auto string_builder = std::make_shared<StringBuilder>();
  ASSERT_TRUE(string_builder->Append("BASE TABLE").ok());
  ASSERT_TRUE(string_builder->Append("VIEW").ok());
  ASSERT_TRUE(string_builder->Append("BASE TABLE").ok());
  ASSERT_TRUE(string_builder->AppendNull().ok());

  std::shared_ptr<Array> string_array;
  ASSERT_TRUE(string_builder->Finish(&string_array).ok());

  auto schema = arrow::schema({field("table_type", utf8())});
  auto original_record_batch = RecordBatch::Make(schema, 4, {string_array});

  // Transform: rename and replace "BASE TABLE" -> "TABLE"
  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .ReplaceFieldValues("table_type", "TABLE_TYPE",
                                             {{"BASE TABLE", "TABLE"}})
                         .Build();

  auto transformed = transformer->Transform(original_record_batch);

  // Verify schema was renamed
  ASSERT_EQ(transformed->schema()->GetFieldIndex("TABLE_TYPE"), 0);
  ASSERT_EQ(transformed->schema()->GetFieldIndex("table_type"), -1);

  // Verify values were replaced
  auto result_array = std::static_pointer_cast<StringArray>(
      transformed->GetColumnByName("TABLE_TYPE"));
  ASSERT_EQ(result_array->length(), 4);
  ASSERT_EQ(result_array->GetString(0), "TABLE");       // "BASE TABLE" -> "TABLE"
  ASSERT_EQ(result_array->GetString(1), "VIEW");         // unchanged
  ASSERT_EQ(result_array->GetString(2), "TABLE");       // "BASE TABLE" -> "TABLE"
  ASSERT_TRUE(result_array->IsNull(3));                   // null stays null
}

TEST(Transformer, CastFieldInt32ToInt16) {
  std::vector<int32_t> values = {1, 2, 3};
  std::shared_ptr<Array> array;
  ArrayFromVector<Int32Type, int32_t>(values, &array);

  auto schema = arrow::schema({field("key_sequence", int32())});
  auto batch = RecordBatch::Make(schema, 3, {array});

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .CastField("key_sequence", "KEY_SEQ", int16())
                         .Build();

  auto transformed = transformer->Transform(batch);

  // Verify schema
  ASSERT_EQ(transformed->schema()->num_fields(), 1);
  ASSERT_EQ(transformed->schema()->field(0)->name(), "KEY_SEQ");
  ASSERT_EQ(transformed->schema()->field(0)->type()->id(), Type::INT16);

  // Verify values
  auto result = std::static_pointer_cast<Int16Array>(
      transformed->GetColumnByName("KEY_SEQ"));
  ASSERT_EQ(result->length(), 3);
  ASSERT_EQ(result->Value(0), 1);
  ASSERT_EQ(result->Value(1), 2);
  ASSERT_EQ(result->Value(2), 3);
}

TEST(Transformer, CastFieldUInt8ToInt16) {
  // Simulates FK update_rule/delete_rule cast (server returns uint8, ODBC expects int16)
  auto builder = std::make_shared<UInt8Builder>();
  ASSERT_TRUE(builder->Append(0).ok());   // CASCADE
  ASSERT_TRUE(builder->Append(1).ok());   // RESTRICT
  ASSERT_TRUE(builder->Append(3).ok());   // SET NULL

  std::shared_ptr<Array> array;
  ASSERT_TRUE(builder->Finish(&array).ok());

  auto schema = arrow::schema({field("update_rule", uint8())});
  auto batch = RecordBatch::Make(schema, 3, {array});

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .CastField("update_rule", "UPDATE_RULE", int16())
                         .Build();

  auto transformed = transformer->Transform(batch);

  ASSERT_EQ(transformed->schema()->field(0)->type()->id(), Type::INT16);

  auto result = std::static_pointer_cast<Int16Array>(
      transformed->GetColumnByName("UPDATE_RULE"));
  ASSERT_EQ(result->Value(0), 0);
  ASSERT_EQ(result->Value(1), 1);
  ASSERT_EQ(result->Value(2), 3);
}

TEST(Transformer, CastFieldPreservesNulls) {
  auto builder = std::make_shared<Int32Builder>();
  ASSERT_TRUE(builder->Append(10).ok());
  ASSERT_TRUE(builder->AppendNull().ok());
  ASSERT_TRUE(builder->Append(30).ok());

  std::shared_ptr<Array> array;
  ASSERT_TRUE(builder->Finish(&array).ok());

  auto schema = arrow::schema({field("value", int32())});
  auto batch = RecordBatch::Make(schema, 3, {array});

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .CastField("value", "VALUE", int16())
                         .Build();

  auto transformed = transformer->Transform(batch);

  auto result = std::static_pointer_cast<Int16Array>(
      transformed->GetColumnByName("VALUE"));
  ASSERT_EQ(result->length(), 3);
  ASSERT_EQ(result->Value(0), 10);
  ASSERT_TRUE(result->IsNull(1));
  ASSERT_EQ(result->Value(2), 30);
}

} // namespace flight_sql
} // namespace driver
