//==============================================================
// Based on Vector Add example from OneAPI samples
//==============================================================
// Copyright © Intel Corporation
// SPDX-License-Identifier: MIT
// =============================================================
#include <gtest/gtest.h>
#include <level_zero/ze_api.h>

#include <chrono>
#include <iostream>
#include <string>
#include <sycl/ext/oneapi/backend/level_zero.hpp>
#include <sycl/sycl.hpp>
#include <thread>
#include <vector>

#include "pti/pti_view.h"

namespace {

enum class TestType {
  kRunAll = 0,
  kOverflowStress = 1,
  kExternalCorrId = 2,
};

constexpr uint32_t kStressLoopCounter = 5;
constexpr size_t kVectorSize = 5000;
constexpr auto kStressWaitTime = std::chrono::seconds{5};

bool matched_sq_corr_ids = false;
bool matched_add_corr_ids = false;
bool timestamps_monotonic = true;
uint64_t sycl_kernel_corr_id[3];
uint64_t sycl_kernel_start_time[3];
uint64_t kernel_corr_id[3];
uint64_t kernel_append_time[3];
uint64_t sycl_idx = 0;
uint64_t kernel_idx = 0;
uint64_t a_append_timestamp = 0;
uint64_t eid = 11;  // external correlation id base.

//************************************
// Vector square in SYCL on device: modifies each input vector
//************************************
template <typename T>
void VecSq(sycl::queue &q, const std::vector<T> &a_vector, const std::vector<T> &b_vector) {
  ASSERT_EQ(ptiViewPushExternalCorrelationId(
                pti_view_external_kind::PTI_VIEW_EXTERNAL_KIND_CUSTOM_3, eid + 20),
            pti_result::PTI_SUCCESS);
  sycl::range<1> num_items{a_vector.size()};
  sycl::buffer a_buf(a_vector);
  sycl::buffer b_buf(b_vector);

  q.submit([&](sycl::handler &h) {
    sycl::accessor a(a_buf, h, sycl::read_write);
    sycl::accessor b(b_buf, h, sycl::read_write);
    h.parallel_for(num_items, [=](auto i) {
      a[i] = a[i] * a[i];
      b[i] = b[i] * b[i];
    });
  });
  q.wait();
  ASSERT_EQ(ptiViewPopExternalCorrelationId(pti_view_external_kind::PTI_VIEW_EXTERNAL_KIND_CUSTOM_3,
                                            &eid),
            pti_result::PTI_SUCCESS);
}

template <typename T>
void VecPassThroughToVecSq(sycl::queue &q, const std::vector<T> &a_vector,
                           const std::vector<T> &b_vector) {
  // This external id(21) is ignored due to overriding push in the VecSq call it preceeds.
  ASSERT_EQ(ptiViewPushExternalCorrelationId(
                pti_view_external_kind::PTI_VIEW_EXTERNAL_KIND_CUSTOM_3, eid + 10),
            pti_result::PTI_SUCCESS);
  VecSq(q, a_vector, b_vector);
  ASSERT_EQ(ptiViewPopExternalCorrelationId(pti_view_external_kind::PTI_VIEW_EXTERNAL_KIND_CUSTOM_3,
                                            &eid),
            pti_result::PTI_SUCCESS);
}

//************************************
// Vector add in SYCL on device: returns sum in 4th parameter "sq_add".
//************************************
template <typename T>
void VecAdd(sycl::queue &q, const std::vector<T> &a_vector, const std::vector<T> &b_vector,
            std::vector<T> &sq_add) {
  sycl::range<1> num_items{a_vector.size()};
  sycl::buffer a_buf(a_vector);
  sycl::buffer b_buf(b_vector);
  sycl::buffer sum_buf(sq_add.data(), num_items);

  q.submit([&](sycl::handler &h) {
    sycl::accessor a(a_buf, h, sycl::read_only);
    sycl::accessor b(b_buf, h, sycl::read_only);
    sycl::accessor sum(sum_buf, h, sycl::write_only, sycl::no_init);
    h.parallel_for(num_items, [=](auto i) { sum[i] = a[i] + b[i]; });
  });
  q.wait();
}

template <typename T>
void PrintResults(const std::vector<T> &sq_add, int n) {
  double sum = 0;
  for (int i = 0; i < n; i++) sum += sq_add[i];
  printf("final result: %f\n", sum / n);
}

void StartTracing() {
  ASSERT_EQ(ptiViewEnable(PTI_VIEW_DEVICE_GPU_KERNEL), pti_result::PTI_SUCCESS);
  ASSERT_EQ(ptiViewEnable(PTI_VIEW_DEVICE_GPU_MEM_COPY), pti_result::PTI_SUCCESS);
  ASSERT_EQ(ptiViewEnable(PTI_VIEW_DEVICE_GPU_MEM_FILL), pti_result::PTI_SUCCESS);
  ASSERT_EQ(ptiViewEnable(PTI_VIEW_SYCL_RUNTIME_CALLS), pti_result::PTI_SUCCESS);
  ASSERT_EQ(ptiViewEnable(PTI_VIEW_EXTERNAL_CORRELATION), pti_result::PTI_SUCCESS);
}

void StopTracing() {
  ASSERT_EQ(ptiViewDisable(PTI_VIEW_DEVICE_GPU_KERNEL), pti_result::PTI_SUCCESS);
  ASSERT_EQ(ptiViewDisable(PTI_VIEW_DEVICE_GPU_MEM_COPY), pti_result::PTI_SUCCESS);
  ASSERT_EQ(ptiViewDisable(PTI_VIEW_DEVICE_GPU_MEM_FILL), pti_result::PTI_SUCCESS);
  ASSERT_EQ(ptiViewDisable(PTI_VIEW_SYCL_RUNTIME_CALLS), pti_result::PTI_SUCCESS);
  ASSERT_EQ(ptiViewDisable(PTI_VIEW_EXTERNAL_CORRELATION), pti_result::PTI_SUCCESS);
}

static void BufferRequested(unsigned char **buf, size_t *buf_size) {
  *buf_size = sizeof(pti_view_record_kernel);
  void *ptr = ::operator new(*buf_size);
  ptr = std::align(8, sizeof(unsigned char), ptr, *buf_size);
  *buf = reinterpret_cast<unsigned char *>(ptr);
  if (!*buf) {
    std::abort();
  }
  return;
}

static void BufferCompleted(unsigned char *buf, size_t buf_size, size_t valid_buf_size) {
  if (!buf_size || !valid_buf_size || !buf_size) {
    std::cerr << "Received empty buffer" << '\n';
    if (valid_buf_size) {
      ::operator delete(buf);
    }
    return;
  }
  pti_view_record_base *ptr = nullptr;
  while (true) {
    auto buf_status = ptiViewGetNextRecord(buf, valid_buf_size, &ptr);
    if (buf_status == pti_result::PTI_STATUS_END_OF_BUFFER) {
      break;
    }
    if (buf_status != pti_result::PTI_SUCCESS) {
      std::cerr << "Found Error Parsing Records from PTI" << '\n';
      break;
    }
    switch (ptr->_view_kind) {
      case pti_view_kind::PTI_VIEW_INVALID: {
        std::cout << "Found Invalid Record" << '\n';
        break;
      }
      case pti_view_kind::PTI_VIEW_EXTERNAL_CORRELATION: {
        [[maybe_unused]] pti_view_record_external_correlation *a_ext_rec =
            reinterpret_cast<pti_view_record_external_correlation *>(ptr);
        break;
      }
      case pti_view_kind::PTI_VIEW_COLLECTION_OVERHEAD: {
        [[maybe_unused]] pti_view_record_overhead *record =
            reinterpret_cast<pti_view_record_overhead *>(ptr);
        break;
      }
      case pti_view_kind::PTI_VIEW_DEVICE_GPU_MEM_COPY: {
        break;
      }
      case pti_view_kind::PTI_VIEW_DEVICE_GPU_MEM_FILL: {
        break;
      }
      case pti_view_kind::PTI_VIEW_SYCL_RUNTIME_CALLS: {
        pti_view_record_sycl_runtime *a_sycl_rec =
            reinterpret_cast<pti_view_record_sycl_runtime *>(ptr);
        std::string function_name = a_sycl_rec->_name;
        if ((sycl_idx < 2) && (function_name.find("urEnqueueKernelLaunch") != std::string::npos)) {
          sycl_kernel_corr_id[sycl_idx] = a_sycl_rec->_correlation_id;
          sycl_kernel_start_time[sycl_idx] = a_sycl_rec->_start_timestamp;
          sycl_idx++;
        };
        break;
      }
      case pti_view_kind::PTI_VIEW_DEVICE_GPU_KERNEL: {
        pti_view_record_kernel *a_kernel_rec = reinterpret_cast<pti_view_record_kernel *>(ptr);
        std::string kernel_name = a_kernel_rec->_name;
        std::cout << "Found Kernel: " << kernel_name << '\n';
        if ((kernel_idx < 2) && (kernel_name.find("VecSq<") != std::string::npos)) {
          kernel_corr_id[kernel_idx] = a_kernel_rec->_correlation_id;
          kernel_append_time[kernel_idx] = a_kernel_rec->_append_timestamp;
          kernel_idx++;
        }
        if ((kernel_idx < 2) && (kernel_name.find("VecAdd<") != std::string::npos)) {
          kernel_corr_id[kernel_idx] = a_kernel_rec->_correlation_id;
          kernel_append_time[kernel_idx] = a_kernel_rec->_append_timestamp;
          kernel_idx++;
        }
        std::cout << "KernelTimestamp for VecAdd: " << a_kernel_rec->_append_timestamp << "\n";
        timestamps_monotonic =
            timestamps_monotonic && (a_append_timestamp < a_kernel_rec->_append_timestamp);
        a_append_timestamp = a_kernel_rec->_append_timestamp;
        break;
      }
      default: {
        std::cerr << "This shouldn't happen" << '\n';
        break;
      }
    }
  }
  ::operator delete(buf);
}

template <typename T>
void RunExternalCorrIdTest(sycl::queue &q, const std::vector<T> &a, const std::vector<T> &b,
                           const std::vector<T> &c, const std::vector<T> &d, std::vector<T> &sq_add,
                           std::vector<T> &sq_add2) {
  StartTracing();
  VecPassThroughToVecSq(q, a, b);
  StopTracing();
  VecAdd(q, a, b, sq_add);
  PrintResults(sq_add, kVectorSize);

  VecAdd(q, a, b, sq_add);
  PrintResults(sq_add, kVectorSize);

  StartTracing();
  VecAdd(q, c, d, sq_add2);
  StopTracing();
  PrintResults(sq_add2, 2 * kVectorSize);
}

template <typename T>
void RunOverflowStressTest(sycl::queue &q, [[maybe_unused]] const std::vector<T> &a,
                           [[maybe_unused]] const std::vector<T> &b, const std::vector<T> &c,
                           const std::vector<T> &d, [[maybe_unused]] std::vector<T> &sq_add,
                           std::vector<T> &sq_add2) {
  StartTracing();
  // kStressLoopCounter value of 17500 works to drive the collection time to 24hours+.
  // TODO: Do we really need this? As it stands, this isn't really a stress test.
  for (uint32_t i = 0; i < kStressLoopCounter; i++) {
    VecAdd(q, c, d, sq_add2);
    std::this_thread::sleep_for(kStressWaitTime);
  }
  StopTracing();
}

template <typename T>
void VecSqAddRouter(sycl::queue &sycl_queue, TestType a_test_type) {
  ASSERT_EQ(ptiViewPushExternalCorrelationId(
                pti_view_external_kind::PTI_VIEW_EXTERNAL_KIND_CUSTOM_3, eid),
            pti_result::PTI_SUCCESS);
  std::vector<T> a(kVectorSize);
  std::vector<T> b(kVectorSize);
  std::vector<T> c(2 * kVectorSize);
  std::vector<T> d(2 * kVectorSize);
  std::vector<T> sq_add(kVectorSize);
  std::vector<T> sq_add2(2 * kVectorSize);

  for (size_t i = 0; i < kVectorSize; i++) {
    a[i] = sin(i);
    b[i] = cos(i);
    c[2 * i] = sin(i) * sin(i);
    c[2 * i + 1] = sin(i);
    d[2 * i] = cos(i) * cos(i);
    d[2 * i + 1] = cos(i);
  }

  if (a_test_type == TestType::kRunAll) {
    RunExternalCorrIdTest(sycl_queue, a, b, c, d, sq_add, sq_add2);
    RunOverflowStressTest(sycl_queue, a, b, c, d, sq_add, sq_add2);
  }

  if (a_test_type == TestType::kExternalCorrId) {
    RunExternalCorrIdTest(sycl_queue, a, b, c, d, sq_add, sq_add2);
  }

  if (a_test_type == TestType::kOverflowStress) {
    RunOverflowStressTest(sycl_queue, a, b, c, d, sq_add, sq_add2);
  }

  ASSERT_EQ(ptiViewPopExternalCorrelationId(pti_view_external_kind::PTI_VIEW_EXTERNAL_KIND_CUSTOM_3,
                                            &eid),
            pti_result::PTI_SUCCESS);
}

void RunVecsqadd(TestType a_test_type) {
  auto dev = sycl::device(sycl::gpu_selector_v);

  auto d_selector{sycl::gpu_selector_v};
  sycl::property_list prop{sycl::property::queue::in_order()};
  sycl::queue q(d_selector, prop);

  if (q.get_device().has(sycl::aspect::fp64)) {
    VecSqAddRouter<double>(q, a_test_type);
  } else {
    VecSqAddRouter<float>(q, a_test_type);
  }

  ASSERT_EQ(ptiFlushAllViews(), pti_result::PTI_SUCCESS);
}

}  // namespace

class VecsqaddFixtureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    matched_sq_corr_ids = false;
    matched_add_corr_ids = false;
    timestamps_monotonic = true;
    a_append_timestamp = 0;
  }

  void TearDown() override {
    // Called right before destructor after each test
  }
};

TEST_F(VecsqaddFixtureTest, CorrelationIdsMatchForSq) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunVecsqadd(TestType::kExternalCorrId);
  EXPECT_EQ(sycl_kernel_corr_id[0], kernel_corr_id[0]);
  EXPECT_LE(sycl_kernel_start_time[0], kernel_append_time[0]);
  if ((sycl_kernel_corr_id[0] == kernel_corr_id[0]) &&
      (sycl_kernel_start_time[0] < kernel_append_time[0]))
    matched_sq_corr_ids = true;
  EXPECT_EQ(matched_sq_corr_ids, true);
}

TEST_F(VecsqaddFixtureTest, CorrelationIdsMatchForAdd) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunVecsqadd(TestType::kExternalCorrId);
  EXPECT_EQ(sycl_kernel_corr_id[1], kernel_corr_id[1]);
  EXPECT_LE(sycl_kernel_start_time[1], kernel_append_time[1]);
  if ((sycl_kernel_corr_id[1] == kernel_corr_id[1]) &&
      (sycl_kernel_start_time[1] < kernel_append_time[1]))
    matched_add_corr_ids = true;
  EXPECT_EQ(matched_add_corr_ids, true);
}

TEST_F(VecsqaddFixtureTest, TimestampWrapAroundOnOverflow) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunVecsqadd(TestType::kOverflowStress);
  EXPECT_EQ(timestamps_monotonic, true);
}
