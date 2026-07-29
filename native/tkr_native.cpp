#include <jni.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct HeapBuffer {
  std::uint8_t* data;
  std::size_t len;
};

std::uint32_t Fnv1a32(const std::uint8_t* data, std::size_t len) {
  std::uint32_t hash = 2166136261u;
  for (std::size_t i = 0; i < len; ++i) {
    hash ^= static_cast<std::uint32_t>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

void TouchBytes(const std::uint8_t* data, std::size_t len) {
  volatile std::uint8_t sink = 0;
  for (std::size_t i = 0; i < len; ++i) {
    sink ^= data[i];
  }
  (void)sink;
}

struct DeferredSlot {
  std::uint32_t record_id;
  const std::uint8_t* payload_ptr;
  std::uint32_t payload_len;
  bool active;
};

struct ChannelEntry {
  const std::uint8_t* payload_ptr;
  std::uint32_t payload_len;
};

struct MergeSlot {
  std::uint32_t leg_id;
  const std::uint8_t* ref_ptr;
  std::uint32_t ref_len;
  bool pinned;
};

std::vector<DeferredSlot> g_batch_slots;
std::vector<ChannelEntry> g_channel_entries;
std::vector<MergeSlot> g_merge_slots;
std::vector<HeapBuffer> g_heap_buffers;

void FreeAllHeapBuffers() {
  for (HeapBuffer& buf : g_heap_buffers) {
    if (buf.data != nullptr) {
      std::free(buf.data);
      buf.data = nullptr;
      buf.len = 0;
    }
  }
  g_heap_buffers.clear();
}

HeapBuffer* StoreBytes(const std::uint8_t* bytes, std::size_t len) {
  HeapBuffer slot{};
  slot.len = len;
  slot.data = static_cast<std::uint8_t*>(std::malloc(len > 0 ? len : 1));
  if (slot.data == nullptr) {
    return nullptr;
  }
  if (len > 0) {
    std::memcpy(slot.data, bytes, len);
  }
  g_heap_buffers.push_back(slot);
  return &g_heap_buffers.back();
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
  return JNI_VERSION_1_8;
}

JNIEXPORT void JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeRegisterBatchSlots(
    JNIEnv* env, jclass, jlongArray ptrs, jintArray lens, jintArray record_ids) {
  g_batch_slots.clear();
  jsize n = env->GetArrayLength(ptrs);
  jlong* ptr_arr = env->GetLongArrayElements(ptrs, nullptr);
  jint* len_arr = env->GetIntArrayElements(lens, nullptr);
  jint* id_arr = env->GetIntArrayElements(record_ids, nullptr);
  for (jsize i = 0; i < n; ++i) {
    DeferredSlot slot{};
    slot.record_id = static_cast<std::uint32_t>(id_arr[i]);
    slot.payload_ptr = reinterpret_cast<const std::uint8_t*>(ptr_arr[i]);
    slot.payload_len = static_cast<std::uint32_t>(len_arr[i]);
    slot.active = slot.payload_ptr != nullptr && slot.payload_len > 0;
    g_batch_slots.push_back(slot);
  }
  env->ReleaseLongArrayElements(ptrs, ptr_arr, JNI_ABORT);
  env->ReleaseIntArrayElements(lens, len_arr, JNI_ABORT);
  env->ReleaseIntArrayElements(record_ids, id_arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeCompactBatchPayload(
    JNIEnv* env, jclass, jbyteArray payload) {
  jsize len = env->GetArrayLength(payload);
  jbyte* bytes = env->GetByteArrayElements(payload, nullptr);
  StoreBytes(reinterpret_cast<const std::uint8_t*>(bytes),
             static_cast<std::size_t>(len));
  env->ReleaseByteArrayElements(payload, bytes, JNI_ABORT);
  // Intentional: invalidate backing storage while deferred batch slots remain.
  FreeAllHeapBuffers();
}

JNIEXPORT jint JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeFlushBatchDigest(JNIEnv*, jclass) {
  if (!g_batch_slots.empty()) {
    FreeAllHeapBuffers();
  }
  std::uint32_t digest = 2166136261u;
  for (const DeferredSlot& slot : g_batch_slots) {
    if (!slot.active || slot.payload_ptr == nullptr || slot.payload_len == 0) {
      continue;
    }
    TouchBytes(slot.payload_ptr, slot.payload_len);
    digest ^= Fnv1a32(slot.payload_ptr, slot.payload_len);
    digest *= 16777619u;
    digest ^= slot.record_id;
  }
  return static_cast<jint>(digest);
}

JNIEXPORT void JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeQueueChannelEntry(
    JNIEnv*, jclass, jlong ptr, jint len) {
  ChannelEntry entry{};
  entry.payload_ptr = reinterpret_cast<const std::uint8_t*>(ptr);
  entry.payload_len = static_cast<std::uint32_t>(len);
  g_channel_entries.push_back(entry);
}

JNIEXPORT void JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeCommitIngressSweep(JNIEnv*, jclass) {
  FreeAllHeapBuffers();
}

JNIEXPORT void JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeStoreHeapBuffer(
    JNIEnv* env, jclass, jbyteArray data) {
  jsize len = env->GetArrayLength(data);
  jbyte* bytes = env->GetByteArrayElements(data, nullptr);
  StoreBytes(reinterpret_cast<const std::uint8_t*>(bytes),
             static_cast<std::size_t>(len));
  env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
}

JNIEXPORT jlong JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeHeapBufferPtr(JNIEnv*, jclass, jint index) {
  if (index < 0 || static_cast<std::size_t>(index) >= g_heap_buffers.size()) {
    return 0;
  }
  return reinterpret_cast<jlong>(g_heap_buffers[static_cast<std::size_t>(index)].data);
}

JNIEXPORT jint JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeHeapBufferCountNative(JNIEnv*, jclass) {
  return static_cast<jint>(g_heap_buffers.size());
}

JNIEXPORT jint JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeSealDeferredEnvelope(JNIEnv*, jclass) {
  if (!g_channel_entries.empty()) {
    FreeAllHeapBuffers();
  }
  std::uint32_t seal = 2166136261u;
  for (const ChannelEntry& entry : g_channel_entries) {
    if (entry.payload_ptr == nullptr || entry.payload_len == 0) {
      continue;
    }
    TouchBytes(entry.payload_ptr, entry.payload_len);
    seal ^= Fnv1a32(entry.payload_ptr, entry.payload_len);
    seal *= 16777619u;
  }
  return static_cast<jint>(seal);
}

JNIEXPORT void JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeRegisterMergeSlots(
    JNIEnv* env, jclass, jlongArray ptrs, jintArray lens, jintArray leg_ids) {
  g_merge_slots.clear();
  jsize n = env->GetArrayLength(ptrs);
  jlong* ptr_arr = env->GetLongArrayElements(ptrs, nullptr);
  jint* len_arr = env->GetIntArrayElements(lens, nullptr);
  jint* id_arr = env->GetIntArrayElements(leg_ids, nullptr);
  for (jsize i = 0; i < n; ++i) {
    MergeSlot slot{};
    slot.leg_id = static_cast<std::uint32_t>(id_arr[i]);
    slot.ref_ptr = reinterpret_cast<const std::uint8_t*>(ptr_arr[i]);
    slot.ref_len = static_cast<std::uint32_t>(len_arr[i]);
    slot.pinned = slot.ref_ptr != nullptr && slot.ref_len > 0;
    g_merge_slots.push_back(slot);
  }
  env->ReleaseLongArrayElements(ptrs, ptr_arr, JNI_ABORT);
  env->ReleaseIntArrayElements(lens, len_arr, JNI_ABORT);
  env->ReleaseIntArrayElements(leg_ids, id_arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeGraftSessionLegs(
    JNIEnv* env, jclass, jbyteArray ref_blob) {
  jsize len = env->GetArrayLength(ref_blob);
  jbyte* bytes = env->GetByteArrayElements(ref_blob, nullptr);
  StoreBytes(reinterpret_cast<const std::uint8_t*>(bytes),
             static_cast<std::size_t>(len));
  env->ReleaseByteArrayElements(ref_blob, bytes, JNI_ABORT);
  // Intentional: merge slots still reference prior ref bytes freed here.
  FreeAllHeapBuffers();
}

JNIEXPORT jint JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeFlushMergeDigest(JNIEnv*, jclass) {
  if (!g_merge_slots.empty()) {
    FreeAllHeapBuffers();
  }
  std::uint32_t digest = 2166136261u;
  for (const MergeSlot& slot : g_merge_slots) {
    if (!slot.pinned || slot.ref_ptr == nullptr || slot.ref_len == 0) {
      continue;
    }
    TouchBytes(slot.ref_ptr, slot.ref_len);
    digest ^= Fnv1a32(slot.ref_ptr, slot.ref_len);
    digest *= 16777619u;
    digest ^= slot.leg_id;
  }
  return static_cast<jint>(digest);
}

JNIEXPORT void JNICALL
Java_com_tkr_nativelink_NativeBridge_nativeResetStateNative(JNIEnv*, jclass) {
  g_batch_slots.clear();
  g_channel_entries.clear();
  g_merge_slots.clear();
  FreeAllHeapBuffers();
}

}  // extern "C"
