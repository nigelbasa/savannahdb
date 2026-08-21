#include "bindings.h"

#include "savannah/index/manager.h"
#include "savannah/storage/vector_iterator.h"

#include <bson/bson.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace savannah::binding {

namespace {

// Build the "<db>.<collection>" namespace string used throughout the wire.
std::string make_ns(std::string_view db, std::string_view coll) {
  std::string ns;
  ns.reserve(db.size() + 1 + coll.size());
  ns.append(db);
  ns.push_back('.');
  ns.append(coll);
  return ns;
}

// Drain up to `batch_size` matches into `out`. Returns true if the iterator
// has at least one more match after the drain (drain-then-peek). Caller uses
// that bool to decide between cursorId=0 and registering for getMore.
bool drain_batch(jungle::storage::v1::Iterator& iter, std::size_t batch_size,
                 std::vector<bson::BsonView>& out) {
  out.reserve(batch_size);
  while (out.size() < batch_size && iter.has_next()) {
    out.push_back(iter.next());
  }
  return iter.has_next();
}

template <typename F>
Napi::Value with_native_guard(Napi::Env env, F&& fn) {
  try {
    return fn();
  } catch (const std::exception& ex) {
    Napi::Error::New(env, ex.what()).ThrowAsJavaScriptException();
    return env.Null();
  } catch (...) {
    Napi::Error::New(env, "unexpected native error")
        .ThrowAsJavaScriptException();
    return env.Null();
  }
}

// Counts can in theory exceed 2^53 (the largest integer JS Number can
// represent without precision loss). Emit BigInt above that boundary so
// callers don't silently get a rounded count. Below the boundary we stay
// on Number — BigInt is awkward to use casually in JS and every value we
// care about today fits comfortably in safe-integer range.
Napi::Value safe_count(Napi::Env env, std::uint64_t value) {
  constexpr std::uint64_t kSafe = (1ULL << 53) - 1;
  if (value <= kSafe) {
    return Napi::Number::New(env, static_cast<double>(value));
  }
  return Napi::BigInt::New(env, value);
}

Napi::Array views_to_array(Napi::Env env,
                            const std::vector<bson::BsonView>& docs) {
  Napi::Array arr = Napi::Array::New(env, docs.size());
  for (std::size_t i = 0; i < docs.size(); ++i) {
    arr.Set(static_cast<std::uint32_t>(i),
            Napi::Buffer<std::uint8_t>::Copy(env, docs[i].data(),
                                              docs[i].size()));
  }
  return arr;
}

Napi::Value Insert(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() ||
        !info[2].IsArray()) {
      Napi::TypeError::New(env, "insert(db: string, coll: string, docs: Buffer[])")
          .ThrowAsJavaScriptException();
      return env.Null();
    }

    const std::string db = info[0].As<Napi::String>().Utf8Value();
    const std::string coll = info[1].As<Napi::String>().Utf8Value();
    Napi::Array arr = info[2].As<Napi::Array>();

    std::vector<savannah::bson::BsonView> views;
    views.reserve(arr.Length());
    for (std::uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value v = arr.Get(i);
      if (!v.IsBuffer()) {
        Napi::TypeError::New(env, "each document must be a Buffer")
            .ThrowAsJavaScriptException();
        return env.Null();
      }
      auto buf = v.As<Napi::Buffer<std::uint8_t>>();
      views.emplace_back(
          std::span<const std::uint8_t>{buf.Data(), buf.Length()});
    }

    auto& collection = global_engine().backend().collection(db, coll);
    auto result = collection.insert(
        std::span<const savannah::bson::BsonView>{views.data(), views.size()});

    Napi::Object out = Napi::Object::New(env);
    out.Set("insertedCount", safe_count(env, result.inserted_count));
    if (result.err_code != 0) {
      Napi::Object err = Napi::Object::New(env);
      err.Set("code", Napi::Number::New(env, result.err_code));
      err.Set("name", Napi::String::New(env, result.err_name));
      err.Set("message", Napi::String::New(env, result.err_message));
      out.Set("err", err);
    }
    return out;
  });
}

Napi::Array index_infos_to_array(Napi::Env env,
                                 const std::vector<index::IndexInfo>& indexes) {
  Napi::Array arr = Napi::Array::New(env, indexes.size());
  for (std::size_t i = 0; i < indexes.size(); ++i) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("name", Napi::String::New(env, indexes[i].name));
    // Always expose fieldPaths (array). For single-field indexes the legacy
    // fieldPath string stays populated; for compound it's an empty string.
    obj.Set("fieldPath", Napi::String::New(env, indexes[i].field_path));
    Napi::Array paths = Napi::Array::New(env, indexes[i].field_paths.size());
    for (std::size_t j = 0; j < indexes[i].field_paths.size(); ++j) {
      paths.Set(static_cast<std::uint32_t>(j),
                Napi::String::New(env, indexes[i].field_paths[j]));
    }
    obj.Set("fieldPaths", paths);
    obj.Set("entries", safe_count(env, indexes[i].entries));
    arr.Set(static_cast<std::uint32_t>(i), obj);
  }
  return arr;
}

// createIndex(db, coll, name, fieldPath: string | string[]) -> { created }
Napi::Value CreateIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 4 || !info[0].IsString() || !info[1].IsString() ||
        !info[2].IsString() ||
        !(info[3].IsString() || info[3].IsArray())) {
      Napi::TypeError::New(
          env,
          "createIndex(db, coll, name, fieldPath | fieldPaths)")
          .ThrowAsJavaScriptException();
      return env.Null();
    }

    const std::string db = info[0].As<Napi::String>().Utf8Value();
    const std::string coll = info[1].As<Napi::String>().Utf8Value();
    const std::string name = info[2].As<Napi::String>().Utf8Value();

    std::vector<std::string> field_paths;
    if (info[3].IsArray()) {
      Napi::Array arr = info[3].As<Napi::Array>();
      field_paths.reserve(arr.Length());
      for (std::uint32_t i = 0; i < arr.Length(); ++i) {
        Napi::Value v = arr.Get(i);
        if (!v.IsString()) {
          Napi::TypeError::New(env, "fieldPaths must be an array of strings")
              .ThrowAsJavaScriptException();
          return env.Null();
        }
        field_paths.push_back(v.As<Napi::String>().Utf8Value());
      }
    } else {
      field_paths.push_back(info[3].As<Napi::String>().Utf8Value());
    }

    auto& collection = global_engine().backend().collection(db, coll);
    const auto created = collection.create_index(
        name, std::span<const std::string>(field_paths));

    Napi::Object out = Napi::Object::New(env);
    out.Set("created", Napi::Boolean::New(env, created.changed));
    if (created.err_code != 0) {
      Napi::Object err = Napi::Object::New(env);
      err.Set("code", Napi::Number::New(env, created.err_code));
      err.Set("name", Napi::String::New(env, created.err_name));
      err.Set("message", Napi::String::New(env, created.err_message));
      out.Set("err", err);
    }
    return out;
  });
}

// dropIndex(db, coll, name) -> { dropped }
Napi::Value DropIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() ||
        !info[2].IsString()) {
      Napi::TypeError::New(env,
                           "dropIndex(db: string, coll: string, name: string)")
          .ThrowAsJavaScriptException();
      return env.Null();
    }

    const std::string db = info[0].As<Napi::String>().Utf8Value();
    const std::string coll = info[1].As<Napi::String>().Utf8Value();
    const std::string name = info[2].As<Napi::String>().Utf8Value();

    auto& collection = global_engine().backend().collection(db, coll);
    const auto dropped = collection.drop_index(name);

    Napi::Object out = Napi::Object::New(env);
    out.Set("dropped", Napi::Boolean::New(env, dropped.changed));
    if (dropped.err_code != 0) {
      Napi::Object err = Napi::Object::New(env);
      err.Set("code", Napi::Number::New(env, dropped.err_code));
      err.Set("name", Napi::String::New(env, dropped.err_name));
      err.Set("message", Napi::String::New(env, dropped.err_message));
      out.Set("err", err);
    }
    return out;
  });
}

// listIndexes(db, coll) -> Array<{ name, fieldPath, entries }>
Napi::Value ListIndexes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
      Napi::TypeError::New(env, "listIndexes(db: string, coll: string)")
          .ThrowAsJavaScriptException();
      return env.Null();
    }

    const std::string db = info[0].As<Napi::String>().Utf8Value();
    const std::string coll = info[1].As<Napi::String>().Utf8Value();

    auto& collection = global_engine().backend().collection(db, coll);
    return index_infos_to_array(env, collection.indexes().list());
  });
}

// find(db, coll, filter, batchSize, sortSpec, skip, limit, projection)
//   -> { batch: Buffer[], cursorId: BigInt }
// sortSpec/projection are BSON docs; pass an empty 5-byte doc to skip them.
// Sorting/skip/limit planning now lives in the engine-side Collection::find.
// Projection still wraps last in the binding so getMore keeps projecting.
Napi::Value Find(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 8 || !info[0].IsString() || !info[1].IsString() ||
        !info[2].IsBuffer() || !info[3].IsNumber() || !info[4].IsBuffer() ||
        !info[5].IsNumber() || !info[6].IsNumber() || !info[7].IsBuffer()) {
      Napi::TypeError::New(
          env,
          "find(db, coll, filter, batchSize, sort, skip, limit, projection)")
          .ThrowAsJavaScriptException();
      return env.Null();
    }

    const std::string db = info[0].As<Napi::String>().Utf8Value();
    const std::string coll = info[1].As<Napi::String>().Utf8Value();
    auto filter = info[2].As<Napi::Buffer<std::uint8_t>>();
    const std::int64_t batch_size_signed =
        info[3].As<Napi::Number>().Int64Value();
    const std::size_t batch_size =
        batch_size_signed > 0 ? static_cast<std::size_t>(batch_size_signed) : 0;
    auto sort_spec = info[4].As<Napi::Buffer<std::uint8_t>>();
    const std::int64_t skip_signed = info[5].As<Napi::Number>().Int64Value();
    const std::size_t skip =
        skip_signed > 0 ? static_cast<std::size_t>(skip_signed) : 0;
    const std::int64_t limit_signed = info[6].As<Napi::Number>().Int64Value();
    const std::size_t limit =
        limit_signed > 0 ? static_cast<std::size_t>(limit_signed) : 0;
    auto projection = info[7].As<Napi::Buffer<std::uint8_t>>();
    const std::span<const std::uint8_t> projection_span{projection.Data(),
                                                         projection.Length()};
    const bool has_projection =
        jungle::query::v1::has_projection(projection_span);

    auto& engine = global_engine();
    auto& collection = engine.backend().collection(db, coll);
    auto iter = collection.find(
        std::span<const std::uint8_t>{filter.Data(), filter.Length()},
        std::span<const std::uint8_t>{sort_spec.Data(), sort_spec.Length()},
        skip, limit);

    if (has_projection) {
      iter = std::make_unique<storage::ProjectingIterator>(std::move(iter),
                                                            projection_span);
    }

    std::vector<bson::BsonView> batch;
    const bool more = drain_batch(*iter, batch_size, batch);

    std::int64_t cursor_id = 0;
    if (more) {
      cursor_id = engine.cursors().register_cursor(
          std::move(iter), make_ns(db, coll));
    }

    Napi::Object out = Napi::Object::New(env);
    out.Set("batch", views_to_array(env, batch));
    out.Set("cursorId", Napi::BigInt::New(env, cursor_id));
    return out;
  });
}

// aggregate(db, coll, pipeline, batchSize) -> { batch: Buffer[], cursorId: BigInt }
// `pipeline` is a BSON wrapper doc of the form `{ pipeline: [ ... ] }`.
Napi::Value Aggregate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 4 || !info[0].IsString() || !info[1].IsString() ||
        !info[2].IsBuffer() || !info[3].IsNumber()) {
      Napi::TypeError::New(
          env, "aggregate(db, coll, pipeline: Buffer, batchSize: number)")
          .ThrowAsJavaScriptException();
      return env.Null();
    }

    const std::string db = info[0].As<Napi::String>().Utf8Value();
    const std::string coll = info[1].As<Napi::String>().Utf8Value();
    auto pipeline = info[2].As<Napi::Buffer<std::uint8_t>>();
    const std::int64_t batch_size_signed =
        info[3].As<Napi::Number>().Int64Value();
    const std::size_t batch_size =
        batch_size_signed > 0 ? static_cast<std::size_t>(batch_size_signed) : 0;

    auto& engine = global_engine();
    auto& collection = engine.backend().collection(db, coll);
    auto iter = collection.aggregate(
        std::span<const std::uint8_t>{pipeline.Data(), pipeline.Length()});

    std::vector<bson::BsonView> batch;
    const bool more = drain_batch(*iter, batch_size, batch);

    std::int64_t cursor_id = 0;
    if (more) {
      cursor_id = engine.cursors().register_cursor(
          std::move(iter), make_ns(db, coll));
    }

    Napi::Object out = Napi::Object::New(env);
    out.Set("batch", views_to_array(env, batch));
    out.Set("cursorId", Napi::BigInt::New(env, cursor_id));
    return out;
  });
}

// getMore(cursorId: BigInt, db, coll, batchSize)
//   -> { batch: Buffer[], cursorId: BigInt }
// Returns cursorId=0n if exhausted. If cursorId is unknown OR the registered
// ns doesn't match db.coll, throws so TS can map it to CursorNotFound (43).
Napi::Value GetMore(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 4 || !info[0].IsBigInt() || !info[1].IsString() ||
        !info[2].IsString() || !info[3].IsNumber()) {
      Napi::TypeError::New(
          env,
          "getMore(cursorId: BigInt, db: string, coll: string, batchSize: number)")
          .ThrowAsJavaScriptException();
      return env.Null();
    }

    bool lossless = false;
    const std::int64_t cursor_id =
        info[0].As<Napi::BigInt>().Int64Value(&lossless);
    const std::string db = info[1].As<Napi::String>().Utf8Value();
    const std::string coll = info[2].As<Napi::String>().Utf8Value();
    const std::int64_t batch_size_signed =
        info[3].As<Napi::Number>().Int64Value();
    const std::size_t batch_size =
        batch_size_signed > 0 ? static_cast<std::size_t>(batch_size_signed) : 0;

    auto& engine = global_engine();
    const std::string ns = make_ns(db, coll);
    auto* iter = engine.cursors().borrow(cursor_id, ns);
    if (!iter) {
      Napi::Error::New(env, "CursorNotFound").ThrowAsJavaScriptException();
      return env.Null();
    }

    std::vector<bson::BsonView> batch;
    const bool more = drain_batch(*iter, batch_size, batch);

    // Copy the batch into JS *before* releasing the cursor. These BsonViews
    // can point into storage owned by the iterator itself -- ProjectingIterator
    // holds projected documents in its own deque, and OwnedBytesIterator (the
    // aggregation path) owns its buffers -- so erase() frees the bytes that
    // views_to_array is about to read. Plain find cursors are unaffected;
    // their views point at arena-owned slot bytes that outlive the iterator.
    Napi::Object out = Napi::Object::New(env);
    out.Set("batch", views_to_array(env, batch));

    std::int64_t next_id = cursor_id;
    if (!more) {
      engine.cursors().erase(cursor_id);
      next_id = 0;
    }
    out.Set("cursorId", Napi::BigInt::New(env, next_id));
    return out;
  });
}

// update(db, coll, filter, spec, multi, upsert)
//   -> { matched, modified, upsertedIds: Buffer[], err? }
// `upsertedIds` are BSON `{_id: <value>}` envelopes; the caller deserializes
// each and reads .['_id'] without us reimplementing libbson value encoding.
Napi::Value Update(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 6 || !info[0].IsString() || !info[1].IsString() ||
        !info[2].IsBuffer() || !info[3].IsBuffer() || !info[4].IsBoolean() ||
        !info[5].IsBoolean()) {
      Napi::TypeError::New(
          env,
          "update(db, coll, filter: Buffer, spec: Buffer, multi: bool, upsert: bool)")
          .ThrowAsJavaScriptException();
      return env.Null();
    }

    const std::string db = info[0].As<Napi::String>().Utf8Value();
    const std::string coll = info[1].As<Napi::String>().Utf8Value();
    auto filter = info[2].As<Napi::Buffer<std::uint8_t>>();
    auto spec = info[3].As<Napi::Buffer<std::uint8_t>>();
    const bool multi = info[4].As<Napi::Boolean>().Value();
    const bool upsert = info[5].As<Napi::Boolean>().Value();

    auto& collection = global_engine().backend().collection(db, coll);
    auto res = collection.update(
        std::span<const std::uint8_t>{filter.Data(), filter.Length()},
        std::span<const std::uint8_t>{spec.Data(), spec.Length()},
        multi, upsert);

    Napi::Object out = Napi::Object::New(env);
    out.Set("matched", safe_count(env, res.matched));
    out.Set("modified", safe_count(env, res.modified));

    Napi::Array upserted = Napi::Array::New(env, res.upserted_ids.size());
    for (std::size_t i = 0; i < res.upserted_ids.size(); ++i) {
      upserted.Set(
          static_cast<std::uint32_t>(i),
          Napi::Buffer<std::uint8_t>::Copy(env, res.upserted_ids[i].data(),
                                            res.upserted_ids[i].size()));
    }
    out.Set("upsertedIds", upserted);

    if (res.err_code != 0) {
      Napi::Object err = Napi::Object::New(env);
      err.Set("code", Napi::Number::New(env, res.err_code));
      err.Set("name", Napi::String::New(env, res.err_name));
      err.Set("message", Napi::String::New(env, res.err_message));
      out.Set("err", err);
    }
    return out;
  });
}

// erase(db, coll, filter, single) -> { deleted }
Napi::Value Erase(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 4 || !info[0].IsString() || !info[1].IsString() ||
        !info[2].IsBuffer() || !info[3].IsBoolean()) {
      Napi::TypeError::New(env,
                           "erase(db, coll, filter: Buffer, single: bool)")
          .ThrowAsJavaScriptException();
      return env.Null();
    }
    const std::string db = info[0].As<Napi::String>().Utf8Value();
    const std::string coll = info[1].As<Napi::String>().Utf8Value();
    auto filter = info[2].As<Napi::Buffer<std::uint8_t>>();
    const bool single = info[3].As<Napi::Boolean>().Value();

    auto& collection = global_engine().backend().collection(db, coll);
    auto res = collection.erase(
        std::span<const std::uint8_t>{filter.Data(), filter.Length()}, single);

    Napi::Object out = Napi::Object::New(env);
    out.Set("deleted", safe_count(env, res.deleted));
    if (res.err_code != 0) {
      Napi::Object err = Napi::Object::New(env);
      err.Set("code", Napi::Number::New(env, res.err_code));
      err.Set("name", Napi::String::New(env, res.err_name));
      err.Set("message", Napi::String::New(env, res.err_message));
      out.Set("err", err);
    }
    return out;
  });
}

// killCursors(ids: BigInt[]) -> { killed: BigInt[], notFound: BigInt[] }
Napi::Value KillCursors(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 1 || !info[0].IsArray()) {
      Napi::TypeError::New(env, "killCursors(ids: BigInt[])")
          .ThrowAsJavaScriptException();
      return env.Null();
    }
    Napi::Array ids = info[0].As<Napi::Array>();
    auto& registry = global_engine().cursors();

    Napi::Array killed = Napi::Array::New(env);
    Napi::Array notFound = Napi::Array::New(env);
    std::uint32_t k = 0, nf = 0;
    for (std::uint32_t i = 0; i < ids.Length(); ++i) {
      Napi::Value v = ids.Get(i);
      if (!v.IsBigInt()) continue;
      bool lossless = false;
      const std::int64_t id = v.As<Napi::BigInt>().Int64Value(&lossless);
      if (registry.erase(id)) {
        killed.Set(k++, Napi::BigInt::New(env, id));
      } else {
        notFound.Set(nf++, Napi::BigInt::New(env, id));
      }
    }

    Napi::Object out = Napi::Object::New(env);
    out.Set("killed", killed);
    out.Set("notFound", notFound);
    return out;
  });
}

// configure(backend: string, root?: string, sync?: string)
//   -> { backend: 'canopy' | 'memory', changed: boolean }
// Selects the storage backend explicitly across the native boundary. This is
// the reliable channel the SDK uses instead of process.env, which on Windows
// does not propagate to the C runtime the engine reads at startup.
Napi::Value Configure(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return with_native_guard(env, [&]() -> Napi::Value {
    if (info.Length() < 1 || !info[0].IsString()) {
      Napi::TypeError::New(env, "configure(backend: string, root?: string)")
          .ThrowAsJavaScriptException();
      return env.Null();
    }
    const std::string backend = info[0].As<Napi::String>().Utf8Value();
    if (backend != "canopy" && backend != "memory") {
      // Don't coerce an unknown string to memory: that would silently discard
      // the caller's data on exit, which is the failure mode this whole
      // configure() channel exists to prevent.
      Napi::TypeError::New(
          env, "configure(): backend must be 'canopy' or 'memory', got '" +
                   backend + "'")
          .ThrowAsJavaScriptException();
      return env.Null();
    }
    savannah::StorageOptions options;
    options.canopy = (backend == "canopy");
    if (info.Length() >= 2 && info[1].IsString()) {
      options.root =
          std::filesystem::path(info[1].As<Napi::String>().Utf8Value());
    }
    if (info.Length() >= 3 && info[2].IsString()) {
      const std::string sync = info[2].As<Napi::String>().Utf8Value();
      if (sync != "full" && sync != "batched") {
        Napi::TypeError::New(
            env, "configure(): sync must be 'batched' or 'full', got '" +
                     sync + "'")
            .ThrowAsJavaScriptException();
        return env.Null();
      }
      options.full_sync = (sync == "full");
    }

    const bool changed = configure_engine(options);

    Napi::Object out = Napi::Object::New(env);
    out.Set("backend",
            Napi::String::New(env, options.canopy ? "canopy" : "memory"));
    out.Set("changed", Napi::Boolean::New(env, changed));
    return out;
  });
}

}  // namespace

void RegisterCollection(Napi::Env env, Napi::Object exports) {
  exports.Set("configure", Napi::Function::New(env, Configure));
  exports.Set("insert", Napi::Function::New(env, Insert));
  exports.Set("createIndex", Napi::Function::New(env, CreateIndex));
  exports.Set("dropIndex", Napi::Function::New(env, DropIndex));
  exports.Set("listIndexes", Napi::Function::New(env, ListIndexes));
  exports.Set("find", Napi::Function::New(env, Find));
  exports.Set("aggregate", Napi::Function::New(env, Aggregate));
  exports.Set("getMore", Napi::Function::New(env, GetMore));
  exports.Set("killCursors", Napi::Function::New(env, KillCursors));
  exports.Set("update", Napi::Function::New(env, Update));
  exports.Set("erase", Napi::Function::New(env, Erase));
}

}  // namespace savannah::binding
