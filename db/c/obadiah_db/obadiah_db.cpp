// Copyright (C) 2019 Petr Fedorov <petr.fedorov@phystech.edu>

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation,  version 2 of the License

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <locale>  // must be before postgres.h to fix /usr/include/libintl.h:39:14: error: expected unqualified-id before ‘const’ error
#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus
#include "postgres.h"
#ifdef __cplusplus
}
#endif  // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus
        //#include <unistd.h>
#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "postgres.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(depth_change_by_episode);
PG_FUNCTION_INFO_V1(spread_by_episode);
PG_FUNCTION_INFO_V1(to_microseconds);
PG_FUNCTION_INFO_V1(CalculateTradingPeriod);
PG_FUNCTION_INFO_V1(SetLogLevel);
PG_FUNCTION_INFO_V1(GetOrderBookQueues);
PG_FUNCTION_INFO_V1(DiscoverPositions);
#ifdef __cplusplus
}
#endif  // __cplusplus
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/frontend_requirements.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "depth.h"
#include "episode.h"
#include "level2.h"
#include "level3.h"
#include "order_book.h"
#include "spi_allocator.h"
// From R
#include "../../../src/base.h"
#include "../../../src/order_book_investigation.h"
#include "../../../src/position_discovery.h"

namespace logging = boost::log;
namespace expr = boost::log::expressions;
BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", obadiah::R::SeverityLevel)

namespace obad {

struct multi_call_context : public obad::postgres_heap {
 multi_call_context() {
  l3 = new (allocation_mode::non_spi) obad::episode{};
  ob = new (allocation_mode::non_spi) obad::order_book{};
  l2 = new (palloc(sizeof(obad::deque<level2>))) obad::deque<level2>{};
  d = new (obad::allocation_mode::non_spi) obad::depth();
 };

 ~multi_call_context() {
  if (l3) delete l3;
  if (ob) delete ob;
  if (l2) {
   l2->~deque();
   pfree(l2);
  }
  if (d) delete d;
 };

 obad::episode *l3;
 obad::order_book *ob;
 obad::depth *d;
 obad::deque<obad::level2> *l2;
};

class elog_sink
    : public sinks::basic_formatted_sink_backend<char,
                                                 sinks::synchronized_feeding> {
public:
 void consume(logging::record_view const &rec, string_type const &msg);
};

void
elog_sink::consume(logging::record_view const &rec, string_type const &msg) {
 logging::value_ref<obadiah::R::SeverityLevel, tag::severity> level =
     rec[severity];
 if (level == obadiah::R::SeverityLevel::kDebug5) {
  elog(DEBUG5, "[5] %s", msg.c_str());
  return;
 }
 if (level == obadiah::R::SeverityLevel::kDebug4) {
  elog(DEBUG4, "[4] %s", msg.c_str());
  return;
 }
 if (level == obadiah::R::SeverityLevel::kDebug3) {
  elog(DEBUG3, "[3] %s", msg.c_str());
  return;
 }
 if (level == obadiah::R::SeverityLevel::kDebug2) {
  elog(DEBUG2, "[2] %s", msg.c_str());
  return;
 }
 if (level == obadiah::R::SeverityLevel::kDebug1) {
  elog(DEBUG1, "[1] %s", msg.c_str());
  return;
 }
 if (level == obadiah::R::SeverityLevel::kInfo) {
  elog(INFO, "%s", msg.c_str());
  return;
 }
 if (level == obadiah::R::SeverityLevel::kNotice) {
  elog(NOTICE, "%s", msg.c_str());
  return;
 }
 if (level == obadiah::R::SeverityLevel::kWarning) {
  elog(WARNING, "%s", msg.c_str());
  return;
 }
 if (level == obadiah::R::SeverityLevel::kError) {
  elog(ERROR, "%s", msg.c_str());
  return;
 }
}
}  // namespace obad

extern "C" void
_PG_init() {
 logging::add_common_attributes();
 /* pid_t pid = getpid();
  char buffer[100];
  sprintf(buffer, "log/obadiah_db_%i.log", pid);
  logging::add_file_log(
      keywords::file_name = buffer,
      keywords::format = "[%TimeStamp%] [%Severity%]: %Message%");
      */
 typedef sinks::synchronous_sink<obad::elog_sink> sink_t;
 boost::shared_ptr<sink_t> sink(new sink_t());
 sink->set_formatter(expr::stream << expr::message);
 logging::core::get()->add_sink(sink);
 logging::core::get()->set_filter(severity >
                                  obadiah::R::SeverityLevel::kNotice);
};

Datum
depth_change_by_episode(PG_FUNCTION_ARGS) {
 using namespace std;
 using namespace obad;

 FuncCallContext *funcctx;
 TupleDesc tupdesc;
 AttInMetadata *attinmeta;

 if (SRF_IS_FIRSTCALL()) {
  MemoryContext oldcontext;

  funcctx = SRF_FIRSTCALL_INIT();

  oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

  if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
   ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                   errmsg("function returning record called in context "
                          "that cannot accept type record")));
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3))
   ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                   errmsg("p_start_time, p_end_time, pair_id, exchange_id must "
                          "not be NULL")));
#if DEBUG_DEPTH
  string p_start_time{timestamptz_to_str(PG_GETARG_TIMESTAMPTZ(0))};
  string p_end_time{timestamptz_to_str(PG_GETARG_TIMESTAMPTZ(1))};
  elog(DEBUG1, "depth_change_by_episode(%s, %s, %i, %i)", p_start_time.c_str(),
       p_end_time.c_str(), PG_GETARG_INT32(2), PG_GETARG_INT32(3));
#endif  // DEBUG_DEPTH
  attinmeta = TupleDescGetAttInMetadata(tupdesc);
  funcctx->attinmeta = attinmeta;
  multi_call_context *d = new (allocation_mode::non_spi) multi_call_context;

  Datum frequency = obad::NULL_FREQ;
  if (!PG_ARGISNULL(4)) frequency = PG_GETARG_DATUM(4);

  d->ob->update(
      d->l3->initial(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), PG_GETARG_DATUM(2),
                     PG_GETARG_DATUM(3), frequency),
      nullptr);  // nullptr == disard level2's generated

  funcctx->user_fctx = d;

  MemoryContextSwitchTo(oldcontext);
 }

 funcctx = SRF_PERCALL_SETUP();

 MemoryContext oldcontext;
 oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

 multi_call_context *d = static_cast<multi_call_context *>(funcctx->user_fctx);
 attinmeta = funcctx->attinmeta;
 try {
  if (d->l2->empty()) {
   while (true) {  // it is possible that some level3 episodes will not generate
                   // level2 changes. We'll skip them
    d->ob->update(d->l3->next(), d->l2);
    if (d->l2->empty()) {
#if DEBUG_DEPTH
     elog(DEBUG2,
          "%s got an empty leve2 result set, proceed to the next episode ...",
          __PRETTY_FUNCTION__);
#endif
    } else {
#if DEBUG_DEPTH
     elog(DEBUG2, "%s got a new level2 result set with %lu level2 records",
          __PRETTY_FUNCTION__, d->l2->size());
#endif
     break;
    }
   }
  }

  level2 result{std::move(d->l2->front())};
  d->l2->pop_front();

#if DEBUG_DEPTH
  elog(DEBUG3, "%s will return %s, remains %lu", __PRETTY_FUNCTION__,
       to_string<level2>(result).c_str(), d->l2->size());
#endif

  MemoryContextSwitchTo(oldcontext);

  HeapTuple tuple_out =
      result.to_heap_tuple(attinmeta, PG_GETARG_INT32(2), PG_GETARG_INT32(3));
  SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple_out));
 } catch (...) {
#if DEBUG_DEPTH
  elog(DEBUG3, "%s ends", __PRETTY_FUNCTION__);
#endif
  MemoryContextSwitchTo(oldcontext);
  delete d;

  SRF_RETURN_DONE(funcctx);
 }
}

Datum
spread_by_episode(PG_FUNCTION_ARGS) {
 using namespace std;
 using namespace obad;

 FuncCallContext *funcctx;
 TupleDesc tupdesc;
 AttInMetadata *attinmeta;

 if (SRF_IS_FIRSTCALL()) {
  MemoryContext oldcontext;

  funcctx = SRF_FIRSTCALL_INIT();

  oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

  if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
   ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                   errmsg("function returning record called in context "
                          "that cannot accept type record")));
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3))
   ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                   errmsg("p_start_time, p_end_time, pair_id, exchange_id must "
                          "not be NULL")));
#if DEBUG_SPREAD
  string p_start_time{timestamptz_to_str(PG_GETARG_TIMESTAMPTZ(0))};
  string p_end_time{timestamptz_to_str(PG_GETARG_TIMESTAMPTZ(1))};
  elog(DEBUG1, "spread_by_episode(%s, %s, %i, %i)", p_start_time.c_str(),
       p_end_time.c_str(), PG_GETARG_INT32(2), PG_GETARG_INT32(3));
#endif  // DEBUG_SPREAD
  attinmeta = TupleDescGetAttInMetadata(tupdesc);
  funcctx->attinmeta = attinmeta;
  multi_call_context *d = new (allocation_mode::non_spi) multi_call_context;

  Datum frequency = obad::NULL_FREQ;
  if (!PG_ARGISNULL(4)) frequency = PG_GETARG_DATUM(4);

  d->ob->update(
      d->l3->initial(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), PG_GETARG_DATUM(2),
                     PG_GETARG_DATUM(3), frequency),
      d->l2);
  d->d->update(d->l2);

  funcctx->user_fctx = d;

  MemoryContextSwitchTo(oldcontext);
 }

 funcctx = SRF_PERCALL_SETUP();

 MemoryContext oldcontext;
 oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

 multi_call_context *d = static_cast<multi_call_context *>(funcctx->user_fctx);
 attinmeta = funcctx->attinmeta;
 try {
  obad::level1 previous{d->d->spread()};
  obad::level1 next{};
  do {
   while (true) {  // it is possible that some level3 episodes will not generate
                   // level2 changes. We'll skip them
    d->ob->update(d->l3->next(), d->l2);
    if (d->l2->empty()) {
#if DEBUG_SPREAD
     elog(DEBUG2,
          "%s got an empty leve2 result set, proceed to the next episode ...",
          __PRETTY_FUNCTION__);
#endif
    } else {
#if DEBUG_SPREAD
     elog(DEBUG2, "%s got a new level2 result set with %lu level2 records",
          __PRETTY_FUNCTION__, d->l2->size());
#endif
     break;
    }
   }
   next = d->d->update(d->l2);
  } while (previous == next);

  MemoryContextSwitchTo(oldcontext);

  SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(next.to_heap_tuple(
                               funcctx->attinmeta, PG_GETARG_INT32(2),
                               PG_GETARG_INT32(3))));
 } catch (...) {
#if DEBUG_SPREAD
  elog(DEBUG3, "%s ends", __PRETTY_FUNCTION__);
#endif
  MemoryContextSwitchTo(oldcontext);
  delete d;

  SRF_RETURN_DONE(funcctx);
 }
}

Datum
to_microseconds(PG_FUNCTION_ARGS) {
 Timestamp arg = PG_GETARG_TIMESTAMP(0);
 PG_RETURN_INT64(arg);
}
namespace obadiah {
namespace postgres {
class DepthChangesStream : public obadiah::R::ObjectStream<obadiah::R::Level2>,
                           public obad::postgres_heap {
public:
 DepthChangesStream(Datum p_start_time, Datum p_end_time, Datum p_pair_id,
                    Datum p_exchange_id, Datum frequency);
 ~DepthChangesStream();
 operator bool();
 DepthChangesStream &operator>>(obadiah::R::Level2 &dc);

private:
 const char *kCursorName = "depth_changes_stream";
 const int kFetchCount = 1000;
 using Cache =
     std::deque<obadiah::R::Level2, obad::spi_allocator<obadiah::R::Level2>>;
 bool state_;
 Cache *cache_;
};

DepthChangesStream::DepthChangesStream(Datum p_start_time, Datum p_end_time,
                                       Datum p_pair_id, Datum p_exchange_id,
                                       Datum p_frequency)
    : state_{true} {
 cache_ = new (SPI_palloc(sizeof(Cache))) Cache;
 Oid types[5];
 Datum values[5];

 types[0] = TIMESTAMPTZOID;
 types[1] = TIMESTAMPTZOID;
 types[2] = INT4OID;
 types[3] = INT4OID;
 types[4] = INTERVALOID;

 values[0] = p_start_time;
 values[1] = p_end_time;
 values[2] = p_pair_id;
 values[3] = p_exchange_id;
 values[4] = p_frequency;

 char nulls[5] = {' ', ' ', ' ', ' ', ' '};
 if (p_frequency == obad::NULL_FREQ) nulls[4] = 'n';

 SPI_cursor_open_with_args(kCursorName, R"QUERY(
        select extract(epoch from timestamp) as timestamp,
               price, volume, side 
        from get.depth($1, $2, $3, $4, $5) order by 1, 2 desc;)QUERY",
                           5, types, values, nulls, true, 0);
}

DepthChangesStream::~DepthChangesStream() {
 cache_->~Cache();
 SPI_pfree(cache_);
 SPI_cursor_close(SPI_cursor_find(kCursorName));
}

DepthChangesStream::operator bool() { return state_; }

DepthChangesStream &
DepthChangesStream::operator>>(obadiah::R::Level2 &dc) {
 // R's NA value.
 // See R source code: src/main/arithmetics.c R_ValueOfNA()
 static const double kR_NaReal = std::nan("1954");
 if (cache_->empty()) {
  SPI_cursor_fetch(SPI_cursor_find(kCursorName), true, kFetchCount);

  if (SPI_processed > 0 && SPI_tuptable != NULL) {
   HeapTuple tuple;
   TupleDesc tupdesc = SPI_tuptable->tupdesc;
   obadiah::R::Level2 l2;

   for (uint64 j = 0; j < SPI_processed; j++) {
    tuple = SPI_tuptable->vals[j];

    l2.s = SPI_getvalue(tuple, tupdesc, SPI_fnumber(tupdesc, "side"))[0] == 'a'
               ? obadiah::R::Side::kAsk
               : obadiah::R::Side::kBid;

    try {
     l2.t.t =
         strtod(SPI_getvalue(tuple, tupdesc, SPI_fnumber(tupdesc, "timestamp")),
                nullptr);
    } catch (...) {
     l2.t.t = kR_NaReal;
    }

    try {
     l2.p = strtod(SPI_getvalue(tuple, tupdesc, SPI_fnumber(tupdesc, "price")),
                   nullptr);
    } catch (...) {
     l2.p = kR_NaReal;
    }

    try {
     l2.v = strtod(SPI_getvalue(tuple, tupdesc, SPI_fnumber(tupdesc, "volume")),
                   nullptr);
    } catch (...) {
     l2.v = kR_NaReal;
    }
    cache_->push_back(l2);
   }
  }
 }
 if (cache_->empty()) {
  state_ = false;
 } else {
  dc = cache_->front();
  cache_->pop_front();
 }
 return *this;
}

class TradingPeriod : public obadiah::R::TradingPeriod<obad::spi_allocator>,
                      public obad::postgres_heap {
public:
 TradingPeriod(obadiah::R::ObjectStream<obadiah::R::Level2> *depth_changes,
               double volume)
     : obadiah::R::TradingPeriod<obad::spi_allocator>{depth_changes, volume} {};

 ~TradingPeriod() { delete depth_changes_; }
};

}  // namespace postgres
}  // namespace obadiah

Datum
CalculateTradingPeriod(PG_FUNCTION_ARGS) {
 using namespace std;
 using namespace obad;

 FuncCallContext *funcctx;
 TupleDesc tupdesc;
 AttInMetadata *attinmeta;

 if (SRF_IS_FIRSTCALL()) {
  MemoryContext oldcontext;

  funcctx = SRF_FIRSTCALL_INIT();

  oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

  if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
   ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                   errmsg("function returning record called in context "
                          "that cannot accept type record")));
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ||
      PG_ARGISNULL(3) || PG_ARGISNULL(4))
   ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                   errmsg("p_start_time, p_end_time, pair_id, exchange_id must "
                          "not be NULL")));
  attinmeta = TupleDescGetAttInMetadata(tupdesc);
  funcctx->attinmeta = attinmeta;

  Datum frequency = obad::NULL_FREQ;
  if (!PG_ARGISNULL(5)) frequency = PG_GETARG_DATUM(5);

  SPI_connect();

  obadiah::postgres::DepthChangesStream *depth_changes_stream =
      new (allocation_mode::spi) obadiah::postgres::DepthChangesStream{
          PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), PG_GETARG_DATUM(2),
          PG_GETARG_DATUM(3), frequency};

  obadiah::postgres::TradingPeriod *trading_period =
      new (allocation_mode::spi) obadiah::postgres::TradingPeriod{
          depth_changes_stream, PG_GETARG_FLOAT8(4)};
  funcctx->user_fctx = trading_period;
  SPI_finish();

  MemoryContextSwitchTo(oldcontext);
 }

 funcctx = SRF_PERCALL_SETUP();

 MemoryContext oldcontext;
 oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
 obadiah::postgres::TradingPeriod *trading_period =
     static_cast<obadiah::postgres::TradingPeriod *>(funcctx->user_fctx);
 attinmeta = funcctx->attinmeta;
 obadiah::R::BidAskSpread output;
 SPI_connect();

 if (*trading_period >> output) {
  SPI_finish();
  MemoryContextSwitchTo(oldcontext);
  get_call_result_type(fcinfo, NULL, &tupdesc);
  tupdesc = BlessTupleDesc(tupdesc);

  Datum values[3];
  bool nulls[3];
  int j = 0;
  std::memset(nulls, 0, sizeof nulls);
  values[j++] =
      TimestampTzGetDatum(std::lround((output.t.t - 946684800.0) * 1000000));
  values[j++] = Float8GetDatum(output.p_bid);
  values[j++] = Float8GetDatum(output.p_ask);
  HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
  SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
  /*
    char **values = (char **)palloc(8 * sizeof(char *));
    static const int BUFFER_SIZE = 128;
    values[0] = (char *)palloc(BUFFER_SIZE * sizeof(char));
    values[1] = (char *)palloc(BUFFER_SIZE * sizeof(char));
    values[2] = (char *)palloc(BUFFER_SIZE * sizeof(char));

    snprintf(values[0], BUFFER_SIZE, "%lu",
             std::lround((output.t.t - 946684800.0) * 1000000));
    snprintf(values[1], BUFFER_SIZE, "%.5f", output.p_bid);
    snprintf(values[2], BUFFER_SIZE, "%.5f", output.p_ask);

    HeapTuple tuple_out = BuildTupleFromCStrings(attinmeta, values);
    pfree(values[0]);
    pfree(values[1]);
    pfree(values[2]);
    pfree(values);

    SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple_out));
    */
 } else {
  delete trading_period;
  SPI_finish();
  MemoryContextSwitchTo(oldcontext);
  SRF_RETURN_DONE(funcctx);
 }
}

Datum
SetLogLevel(PG_FUNCTION_ARGS) {
 text *t = PG_GETARG_TEXT_PP(0);
 try {
  logging::core::get()->set_filter(
      !expr::has_attr(severity) ||
      severity >= obadiah::R::GetSeverityLevel(
                      std::string(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t))));
 } catch (std::out_of_range &) {
  logging::core::get()->set_filter(severity >
                                   obadiah::R::SeverityLevel::kNotice);
 };
 PG_RETURN_NULL();
}

namespace obadiah {
namespace postgres {
using Price = obadiah::R::Price;
using Volume = obadiah::R::Volume;
using TickSizeType = obadiah::R::TickSizeType;

class DepthToQueues
    : public obadiah::R::DepthToQueues<obad::spi_allocator>,
      public obad::postgres_heap {
public:
 using LevelNo = obadiah::R::DepthToQueues<obad::spi_allocator>::LevelNo;
 DepthToQueues(ObjectStream<obadiah::R::Level2> *depth_changes,
                  const Price tick_size, LevelNo first_tick, LevelNo last_tick,
                  std::string tick_type)
     : obadiah::R::DepthToQueues<obad::spi_allocator>(
           depth_changes, tick_size, first_tick, last_tick, tick_type){};
 ~DepthToQueues() { delete depth_changes_; }
};

}  // namespace postgres
}  // namespace obadiah

Datum
GetOrderBookQueues(PG_FUNCTION_ARGS) {
 FuncCallContext *funcctx;
 TupleDesc tupdesc;
 AttInMetadata *attinmeta;

 if (SRF_IS_FIRSTCALL()) {
  MemoryContext oldcontext;

  funcctx = SRF_FIRSTCALL_INIT();

  oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

  if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
   ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                   errmsg("function returning record called in context "
                          "that cannot accept type record")));
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ||
      PG_ARGISNULL(3) || PG_ARGISNULL(4) || PG_ARGISNULL(5) ||
      PG_ARGISNULL(6) || PG_ARGISNULL(7))
   ereport(ERROR,
           (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
            errmsg("p_start_time, p_end_time, pair_id, exchange_id, tick_size, "
                   "first_tick, last_tick, tick_type must not be NULL")));
  attinmeta = TupleDescGetAttInMetadata(tupdesc);
  funcctx->attinmeta = attinmeta;

  Datum frequency = obad::NULL_FREQ;
  if (!PG_ARGISNULL(8)) frequency = PG_GETARG_DATUM(8);

  SPI_connect();

  obadiah::postgres::DepthChangesStream *depth_changes_stream =
      new (obad::allocation_mode::spi) obadiah::postgres::DepthChangesStream{
          PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), PG_GETARG_DATUM(2),
          PG_GETARG_DATUM(3), frequency};

  obadiah::postgres::DepthToQueues *depth_to_snapshots =
      new (obad::allocation_mode::spi) obadiah::postgres::DepthToQueues{
          depth_changes_stream, PG_GETARG_FLOAT8(4), PG_GETARG_INT32(5),
          PG_GETARG_INT32(6),
          std::string{VARDATA_ANY(PG_GETARG_TEXT_PP(7)),
                      VARSIZE_ANY_EXHDR(PG_GETARG_TEXT_PP(7))}};

  funcctx->user_fctx = depth_to_snapshots;
  SPI_finish();

  MemoryContextSwitchTo(oldcontext);
 }

 funcctx = SRF_PERCALL_SETUP();

 MemoryContext oldcontext;
 oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
 obadiah::postgres::DepthToQueues *depth_to_snapshots =
     static_cast<obadiah::postgres::DepthToQueues *>(funcctx->user_fctx);
 attinmeta = funcctx->attinmeta;
 SPI_connect();
 obadiah::R::OrderBookQueues<obad::spi_allocator> output;

 if (*depth_to_snapshots >> output) {
  SPI_finish();
  MemoryContextSwitchTo(oldcontext);
  std::vector<Datum, obad::p_allocator<Datum>> bids;
  std::vector<Datum, obad::p_allocator<Datum>> asks;
  bids.reserve(output.bids.size());
  for (auto v : output.bids) bids.push_back(Float8GetDatum(v));
  asks.reserve(output.asks.size());
  for (auto v : output.asks) asks.push_back(Float8GetDatum(v));
  get_call_result_type(fcinfo, NULL, &tupdesc);
  tupdesc = BlessTupleDesc(tupdesc);

  Datum values[5];
  int16 typlen;
  bool typbyval;
  char typalign;

  get_typlenbyvalalign(FLOAT8OID, &typlen, &typbyval, &typalign);

  bool nulls[5];
  int j = 0;
  std::memset(nulls, 0, sizeof nulls);
  values[j++] =
      TimestampTzGetDatum(std::lround((output.t.t - 946684800.0) * 1000000));
  values[j++] = Float8GetDatum(output.bid_price);
  values[j++] = Float8GetDatum(output.ask_price);
  values[j++] = PointerGetDatum(construct_array(
      bids.data(), bids.size(), FLOAT8OID, typlen, typbyval, typalign));
  values[j++] = PointerGetDatum(construct_array(
      asks.data(), asks.size(), FLOAT8OID, typlen, typbyval, typalign));
  HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
  SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
 } else {
  delete depth_to_snapshots;
  SPI_finish();
  MemoryContextSwitchTo(oldcontext);
  SRF_RETURN_DONE(funcctx);
 }
}

namespace obadiah {
namespace postgres {

class TradingStrategy : public obadiah::R::TradingStrategy,
                        public obad::postgres_heap {
public:
 TradingStrategy(
     obadiah::R::ObjectStream<obadiah::R::BidAskSpread> *trading_period,
     double phi, double rho)
     : obadiah::R::TradingStrategy{trading_period, phi, rho} {};

 ~TradingStrategy() { delete trading_period_; }
};

}  // namespace postgres
}  // namespace obadiah

Datum
DiscoverPositions(PG_FUNCTION_ARGS) {
 FuncCallContext *funcctx;
 TupleDesc tupdesc;
 AttInMetadata *attinmeta;

 if (SRF_IS_FIRSTCALL()) {
  MemoryContext oldcontext;

  funcctx = SRF_FIRSTCALL_INIT();

  oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

  if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
   ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                   errmsg("function returning record called in context "
                          "that cannot accept type record")));
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ||
      PG_ARGISNULL(3) || PG_ARGISNULL(4))
   ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                   errmsg("p_start_time, p_end_time, pair_id, exchange_id must "
                          "not be NULL")));
  attinmeta = TupleDescGetAttInMetadata(tupdesc);
  funcctx->attinmeta = attinmeta;

  Datum frequency = obad::NULL_FREQ;
  if (!PG_ARGISNULL(7)) frequency = PG_GETARG_DATUM(7);

  SPI_connect();

  obadiah::postgres::DepthChangesStream *depth_changes_stream =
      new (obad::allocation_mode::spi) obadiah::postgres::DepthChangesStream{
          PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), PG_GETARG_DATUM(2),
          PG_GETARG_DATUM(3), frequency};

  obadiah::postgres::TradingPeriod *trading_period =
      new (obad::allocation_mode::spi) obadiah::postgres::TradingPeriod{
          depth_changes_stream, PG_GETARG_FLOAT8(4)};

  obadiah::postgres::TradingStrategy *trading_strategy =
      new (obad::allocation_mode::spi) obadiah::postgres::TradingStrategy{
          trading_period, PG_GETARG_FLOAT8(5), PG_GETARG_FLOAT8(6)};
  funcctx->user_fctx = trading_strategy;
  SPI_finish();

  MemoryContextSwitchTo(oldcontext);
 }

 funcctx = SRF_PERCALL_SETUP();

 MemoryContext oldcontext;
 oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
 obadiah::postgres::TradingStrategy *trading_strategy =
     static_cast<obadiah::postgres::TradingStrategy *>(funcctx->user_fctx);
 attinmeta = funcctx->attinmeta;
 obadiah::R::Position output;
 SPI_connect();

 if (*trading_strategy >> output) {
  SPI_finish();
  MemoryContextSwitchTo(oldcontext);
  get_call_result_type(fcinfo, NULL, &tupdesc);
  tupdesc = BlessTupleDesc(tupdesc);

  Datum values[7];
  bool nulls[7];
  int j = 0;
  std::memset(nulls, 0, sizeof nulls);
  values[j++] =
      TimestampTzGetDatum(std::lround((output.s.t.t - 946684800.0) * 1000000));
  values[j++] = Float8GetDatum(output.s.p);
  values[j++] =
      TimestampTzGetDatum(std::lround((output.e.t.t - 946684800.0) * 1000000));
  values[j++] = Float8GetDatum(output.e.p);
  values[j++] = Float8GetDatum(
      output.s.p > output.e.p ? (output.s.p - output.e.p) / output.s.p * 10000
                              : (output.e.p - output.s.p) / output.s.p * 10000);
  double log_return = output.s.p > output.e.p
                          ? std::log(output.s.p) - std::log(output.e.p)
                          : std::log(output.e.p) - std::log(output.s.p);
  values[j++] =
      Float8GetDatum(std::exp(log_return / (output.e.t - output.s.t)) - 1);
  values[j++] = Float8GetDatum(log_return);

  HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
  SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
 } else {
  delete trading_strategy;
  SPI_finish();
  MemoryContextSwitchTo(oldcontext);
  SRF_RETURN_DONE(funcctx);
 }
}

