#include "bindings.h"

#include "savannah/index/manager.h"
#include "savannah/query/sort.h"
#include "savannah/storage/vector_iterator.h"

#include <bson/bson.h>

#include <algorithm>
#include <cstdint>
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
    views.emplace_back(std::span<const std::uint8_t>{buf.Data(), buf.Length()});
  }

  auto& collection = global_engine().backend().collection(db, coll);
  auto result = collection.insert(
      std::span<const savannah::bson::BsonView>{views.data(), views.size()});

  Napi::Object out = Napi::Object::New(env);
  out.Set("insertedCount",
          Napi::Number::New(env, static_cast<double>(result.inserted_count)));
  return out;
}

Napi::Array index_infos_to_array(Napi::Env env,
                                 const std::vector<index::IndexInfo>& indexes) {
  Napi::Array arr = Napi::Array::New(env, indexes.size());
  for (std::size_t i = 0; i < indexes.size(); ++i) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("name", Napi::String::New(env, indexes[i].name));
    obj.Set("fieldPath", Napi::String::New(env, indexes[i].field_path));
    obj.Set("entries",
            Napi::Number::New(env, static_cast<double>(indexes[i].entries)));
    arr.Set(static_cast<std::uint32_t>(i), obj);
  }
  return arr;
}

// createIndex(db, coll, name, fieldPath) -> { created }
Napi::Value CreateIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !info[1].IsString() ||
      !info[2].IsString() || !info[3].IsString()) {
    Napi::TypeError::New(
        env,
        "createIndex(db: string, coll: string, name: string, fieldPath: string)")
        .ThrowAsJavaScriptException();
    return env.Null();
  }

  const std::string db = info[0].As<Napi::String>().Utf8Value();
  const std::string coll = info[1].As<Napi::String>().Utf8Value();
  const std::string name = info[2].As<Napi::String>().Utf8Value();
  const std::string field_path = info[3].As<Napi::String>().Utf8Value();

  auto& collection = global_engine().backend().collection(db, coll);
  const bool created = collection.indexes().create(name, field_path);
  if (created) collection.backfill_index(name);

  Napi::Object out = Napi::Object::New(env);
  out.Set("created", Napi::Boolean::New(env, created));
  return out;
}

// dropIndex(db, coll, name) -> { dropped }
Napi::Value DropIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
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
  const bool dropped = collection.indexes().drop(name);

  Napi::Object out = Napi::Object::New(env);
  out.Set("dropped", Napi::Boolean::New(env, dropped));
  return out;
}

// listIndexes(db, coll) -> Array<{ name, fieldPath, entries }>
Napi::Value ListIndexes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
    Napi::TypeError::New(env, "listIndexes(db: string, coll: string)")
        .ThrowAsJavaScriptException();
    return env.Null();
  }

  const std::string db = info[0].As<Napi::String>().Utf8Value();
  const std::string coll = info[1].As<Napi::String>().Utf8Value();

  auto& collection = global_engine().backend().collection(db, coll);
  return index_infos_to_array(env, collection.indexes().list());
}

// find(db, coll, filter, batchSize, sortSpec, skip, limit, projection)
//   -> { batch: Buffer[], cursorId: BigInt }
// sortSpec/projection are BSON docs; pass an empty 5-byte doc to skip them.
// When any of sort/skip/limit are active we drain the full result, sort+slice
// in memory, then wrap the materialized vector in a VectorIterator. When
// projection is active we wrap whatever iterator results in a ProjectingIterator
// so getMore continues to project. Streaming is preserved for filter-only
// queries (no sort/skip/limit/projection).
Napi::Value Find(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
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
  const std::int64_t batch_size_signed = info[3].As<Napi::Number>().Int64Value();
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

  // An empty 5-byte sort spec ({} BSON) means "no sort". Anything with at
  // least one key triggers the materialize path.
  bool has_sort = false;
  if (sort_spec.Length() >= 5) {
    bson_t s;
    if (bson_init_static(&s, sort_spec.Data(), sort_spec.Length())) {
      bson_iter_t it;
      if (bson_iter_init(&it, &s) && bson_iter_next(&it)) has_sort = true;
    }
  }
  const bool needs_materialize = has_sort || skip > 0 || limit > 0;

  auto& engine = global_engine();
  auto& collection = engine.backend().collection(db, coll);
  auto raw_iter = collection.find(
      std::span<const std::uint8_t>{filter.Data(), filter.Length()});

  std::unique_ptr<jungle::storage::v1::Iterator> iter;
  if (needs_materialize) {
    std::vector<bson::BsonView> all;
    while (raw_iter->has_next()) all.push_back(raw_iter->next());

    if (has_sort) {
      const std::span<const std::uint8_t> spec_span{sort_spec.Data(),
                                                     sort_spec.Length()};
      std::stable_sort(all.begin(), all.end(),
                       [&](const bson::BsonView& a, const bson::BsonView& b) {
                         return jungle::query::v1::sort_less(a, b, spec_span);
                       });
    }
    if (skip > 0) {
      if (skip >= all.size()) all.clear();
      else all.erase(all.begin(), all.begin() + skip);
    }
    if (limit > 0 && all.size() > limit) {
      all.resize(limit);
    }
    iter = std::make_unique<storage::VectorIterator>(std::move(all));
  } else {
    iter = std::move(raw_iter);
  }

  // Projection wraps last so it sees the post-sort/limit stream and rides
  // through getMore via the cursor registry.
  if (has_projection) {
    iter = std::make_unique<storage::ProjectingIterator>(std::move(iter),
                                                          projection_span);
  }

  std::vector<bson::BsonView> batch;
  const bool more = drain_batch(*iter, batch_size, batch);

  std::int64_t cursor_id = 0;
  if (more) {
    cursor_id = engine.cursors().register_cursor(std::move(iter), make_ns(db, coll));
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("batch", views_to_array(env, batch));
  out.Set("cursorId", Napi::BigInt::New(env, cursor_id));
  return out;
}

// getMore(cursorId: BigInt, db, coll, batchSize)
//   -> { batch: Buffer[], cursorId: BigInt }
// Returns cursorId=0n if exhausted. If cursorId is unknown OR the registered
// ns doesn't match db.coll, throws so TS can map it to CursorNotFound (43).
Napi::Value GetMore(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsBigInt() || !info[1].IsString() ||
      !info[2].IsString() || !info[3].IsNumber()) {
    Napi::TypeError::New(
        env, "getMore(cursorId: BigInt, db: string, coll: string, batchSize: number)")
        .ThrowAsJavaScriptException();
    return env.Null();
  }

  bool lossless = false;
  const std::int64_t cursor_id = info[0].As<Napi::BigInt>().Int64Value(&lossless);
  const std::string db = info[1].As<Napi::String>().Utf8Value();
  const std::string coll = info[2].As<Napi::String>().Utf8Value();
  const std::int64_t batch_size_signed = info[3].As<Napi::Number>().Int64Value();
  const std::size_t batch_size =
      batch_size_signed > 0 ? static_cast<std::size_t>(batch_size_signed) : 0;

  auto& engine = global_engine();
  const std::string ns = make_ns(db, coll);
  auto* iter = engine.cursors().borrow(cursor_id, ns);
  if (!iter) {
    // The TS layer turns this into a CommandNotFound-style {ok:0, code:43}.
    Napi::Error::New(env, "CursorNotFound").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::vector<bson::BsonView> batch;
  const bool more = drain_batch(*iter, batch_size, batch);
  std::int64_t next_id = cursor_id;
  if (!more) {
    engine.cursors().erase(cursor_id);
    next_id = 0;
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("batch", views_to_array(env, batch));
  out.Set("cursorId", Napi::BigInt::New(env, next_id));
  return out;
}

// update(db, coll, filter, spec, multi, upsert)
//   -> { matched, modified, upsertedIds: Buffer[], err? }
// `upsertedIds` are BSON `{_id: <value>}` envelopes; the caller deserializes
// each and reads .['_id'] without us reimplementing libbson value encoding.
Napi::Value Update(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
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
  out.Set("matched", Napi::Number::New(env, static_cast<double>(res.matched)));
  out.Set("modified", Napi::Number::New(env, static_cast<double>(res.modified)));

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
}

// erase(db, coll, filter, single) -> { deleted }
Napi::Value Erase(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
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
  out.Set("deleted", Napi::Number::New(env, static_cast<double>(res.deleted)));
  return out;
}

// killCursors(ids: BigInt[]) -> { killed: BigInt[], notFound: BigInt[] }
Napi::Value KillCursors(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
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
}

}  // namespace

void RegisterCollection(Napi::Env env, Napi::Object exports) {
  exports.Set("insert", Napi::Function::New(env, Insert));
  exports.Set("createIndex", Napi::Function::New(env, CreateIndex));
  exports.Set("dropIndex", Napi::Function::New(env, DropIndex));
  exports.Set("listIndexes", Napi::Function::New(env, ListIndexes));
  exports.Set("find", Napi::Function::New(env, Find));
  exports.Set("getMore", Napi::Function::New(env, GetMore));
  exports.Set("killCursors", Napi::Function::New(env, KillCursors));
  exports.Set("update", Napi::Function::New(env, Update));
  exports.Set("erase", Napi::Function::New(env, Erase));
}

}  // namespace savannah::binding
