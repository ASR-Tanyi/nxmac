#include "nxmac/horizon/handles.h"

#include <cstdint>
#include <memory>

#include "gtest/gtest.h"

namespace nxmac::horizon {
namespace {

TEST(HandleTableTest, HandlesFrom0x1001) {
    auto table = std::make_unique<HandleTable>();
    int main_thread = 0;
    int event = 0;
    const auto h1 = table->New(ObjectType::kThread, &main_thread);
    const auto h2 = table->New(ObjectType::kReadableEvent, &event);
    ASSERT_TRUE(h1.ok()) << h1.status();
    ASSERT_TRUE(h2.ok()) << h2.status();
    EXPECT_EQ(*h1, 0x1001u);  // the main thread's handle, as on nxnative's host
    EXPECT_EQ(*h2, 0x1002u);
    EXPECT_EQ(table->Peek(0x1001, ObjectType::kThread), &main_thread);
    EXPECT_EQ(table->Peek(0x1002, ObjectType::kReadableEvent), &event);
}

TEST(HandleTableTest, AWrongHandleOrTypeIsNothing) {
    auto table = std::make_unique<HandleTable>();
    int thread = 0;
    ASSERT_TRUE(table->New(ObjectType::kThread, &thread).ok());
    EXPECT_EQ(table->Peek(0x1001, ObjectType::kReadableEvent), nullptr);
    EXPECT_EQ(table->Peek(0x1001, ObjectType::kFree), nullptr);
    for (const std::uint32_t h : {0u, 0x1000u, 0x1002u, 0x2000u, 0xFFFFu, kCurrentThreadHandle}) {
        EXPECT_EQ(table->Peek(h, ObjectType::kThread), nullptr) << h;
    }
    EXPECT_EQ(table->New(ObjectType::kFree, &thread).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(HandleTableTest, TheTableFills) {
    auto table = std::make_unique<HandleTable>();
    int obj = 0;
    for (std::uint32_t i = 1; i < HandleTable::kSlots; ++i) {
        const auto h = table->New(ObjectType::kThread, &obj);
        ASSERT_TRUE(h.ok()) << i;
        EXPECT_EQ(*h, 0x1000u + i);
    }
    const auto full = table->New(ObjectType::kThread, &obj);
    EXPECT_EQ(full.status().code(), absl::StatusCode::kResourceExhausted);
    EXPECT_EQ(full.status().message(), "handle table full");
    EXPECT_EQ(table->Peek(0x1fff, ObjectType::kThread), &obj);
}

}  // namespace
}  // namespace nxmac::horizon
