/// tests/unit/engine/villa_integration_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Villa integration contract v1 tests                                     ║
// ║    - registry / trigger digests, schedule-name parsing                   ║
// ║    - timer continuity across reloads (FakeClock)                         ║
// ║    - durable occurrences + trusted env propagation                       ║
// ║    - restart recovery (unstarted occurrences → skipped)                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/config/yaml_loader.hpp"
#include "kairos/engine/pipeline.hpp"
#include "kairos/engine/scheduler.hpp"
#include "kairos/engine/villa_integration.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/runner_pool.hpp"
#include "kairos/persist/occurrence_store.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_process.hpp"

#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace kairos;
using namespace kairos::engine;
using namespace std::chrono_literals;

namespace {

const std::string kSid = "sch_0123456789abcdef0123456789abcdef";
std::string sched_name(int gen) {
    return "villa.schedule." + kSid + ".g" + std::to_string(gen);
}

WorkflowDef make_wf(const std::string& name, const std::string& src) {
    std::string id = "wfl-" + villa::sha256_hex(name).substr(0, 12);
    JobDef job{.job_id = id + "-job", .job_name = "invoke"};
    StepDef step{.step_id = "stp-1", .step_name = "dispatch",
                 .command = "/bin/true"};
    job.steps = {step};
    DagNode node{.job_id = job.job_id, .job_name = job.job_name};
    WorkflowDef wf{
        .workflow_id = id,
        .workflow_name = name,
        .jobs = {job},
        .dag = WorkflowDag::build({node}),
    };
    wf.source_sha256 = villa::sha256_hex(src);
    return wf;
}

TimerEntry interval_for(const WorkflowDef& wf, std::chrono::milliseconds iv) {
    TimerEntry e;
    e.trigger_id = "trg-" + wf.workflow_id;
    e.target_id = wf.workflow_id;
    e.target_name = wf.workflow_name;
    e.spec = IntervalTrigger{.interval = iv};
    return e;
}

TimerEntry date_for(const WorkflowDef& wf, SystemTimePoint at) {
    TimerEntry e;
    e.trigger_id = "trg-d-" + wf.workflow_id;
    e.target_id = wf.workflow_id;
    e.target_name = wf.workflow_name;
    e.spec = DateTrigger{.fire_at = at};
    return e;
}

std::shared_ptr<const WorkflowRegistry> make_reg(
    std::vector<WorkflowDef> wfs, std::vector<TimerEntry> trg) {
    return std::make_shared<WorkflowRegistry>(std::move(wfs), std::move(trg));
}

template <typename Pred>
bool wait_for(Pred p, std::chrono::milliseconds timeout = 3000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return p();
}

}  // namespace

// ── Pure helpers ────────────────────────────────────────────────────────

TEST(VillaDigest, EmptyRegistryIsDigestOfEmptyArray) {
    EXPECT_EQ(villa::registry_digest({}),
              "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945");
    WorkflowRegistry empty({}, {});
    EXPECT_EQ(empty.registry_digest(), villa::registry_digest({}));
}

TEST(VillaDigest, CanonicalSortedPairs) {
    // Expected value computed independently (Python):
    //   json.dumps(sorted(pairs), separators=(',',':')) → sha256
    std::string a = villa::sha256_hex("A");
    std::string b = villa::sha256_hex("B");
    auto d1 = villa::registry_digest({
        {"villa.schedule.sch_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.g1", a},
        {"villa.manual.run_x", b}});
    EXPECT_EQ(d1,
        "1fe4d0dba80aca0f6cfe844067db72949f2b6049fc792bee65c7d57a327196c4");

    // Order of input does not matter; registry computes the same value.
    auto reg = make_reg({make_wf("villa.manual.run_x", "B"),
                         make_wf("villa.schedule.sch_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.g1", "A")},
                        {});
    EXPECT_EQ(reg->registry_digest(), d1);
}

TEST(VillaDigest, LoaderHashesExactFileBytes) {
    kairos::testing::TempDir dir;
    const std::string y1 =
        "name: " + sched_name(1) + "\n"
        "triggers:\n  - type: interval\n    interval: 60s\n"
        "jobs:\n  invoke:\n    steps:\n      - name: dispatch\n"
        "        command: /usr/bin/env\n        use_shell: false\n";
    const std::string y2 =
        "name: other\njobs:\n  j:\n    steps:\n      - name: s\n"
        "        command: /bin/true\n        use_shell: false\n";
    dir.create_file(sched_name(1) + ".yaml", y1);
    dir.create_file("other.yaml", y2);

    auto r = config::load_workflows_dir(dir.path());
    ASSERT_TRUE(r.errors.empty()) << r.errors.front().message;
    WorkflowRegistry reg(std::move(r.workflows), std::move(r.triggers));
    auto expected = villa::registry_digest({
        {sched_name(1), villa::sha256_hex(y1)},
        {"other", villa::sha256_hex(y2)},
    });
    EXPECT_EQ(reg.registry_digest(), expected);
    EXPECT_EQ(reg.workflow_by_name("other")->source_sha256,
              villa::sha256_hex(y2));
}

TEST(VillaDigest, TriggerDigestIsCanonicalTimingOnly) {
    EXPECT_EQ(villa::canonical_trigger_json(
                  IntervalTrigger{.interval = 60000ms}),
              R"({"interval_ms":60000,"type":"interval"})");
    EXPECT_EQ(villa::trigger_digest(IntervalTrigger{.interval = 60000ms}),
              "5d163749f945c9f61a8ddede00f11cf913d7f0b82cf77606ad6e512e022d3f57");
    EXPECT_EQ(villa::canonical_trigger_json(CronTrigger{.expression = "0 2 * * *"}),
              R"({"expression":"0 2 * * *","type":"cron"})");
    DateTrigger dt{.fire_at = std::chrono::system_clock::time_point{
        std::chrono::milliseconds(1794646800250LL)}};
    EXPECT_EQ(villa::canonical_trigger_json(dt),
              R"({"at":"2026-11-14T09:00:00.250Z","type":"date"})");
}

TEST(VillaDigest, ScheduleNameParsing) {
    auto id = villa::parse_schedule_name(sched_name(42));
    ASSERT_TRUE(id);
    EXPECT_EQ(id->schedule_id, kSid);
    EXPECT_EQ(id->generation, 42);
    EXPECT_FALSE(villa::parse_schedule_name("villa.manual.run_" + std::string(32, 'a')));
    EXPECT_FALSE(villa::parse_schedule_name("villa.schedule." + kSid + ".g"));
    EXPECT_FALSE(villa::parse_schedule_name("villa.schedule.sch_0123456789ABCDEF0123456789abcdef.g1"));
    EXPECT_FALSE(villa::parse_schedule_name("villa.schedule." + kSid + ".g1x"));
    EXPECT_FALSE(villa::parse_schedule_name("xvilla.schedule." + kSid + ".g1"));
}

TEST(VillaDigest, DateTriggerAcceptsRfc3339Utc) {
    auto r = config::load_workflow_string(
        "name: d\ntriggers:\n  - type: date\n    date: \"2026-11-14T09:00:00.250Z\"\n"
        "jobs:\n  j:\n    steps:\n      - name: s\n        command: /bin/true\n");
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.triggers.size(), 1u);
    auto& dt = std::get<DateTrigger>(r.triggers[0].spec);
    EXPECT_EQ(villa::format_rfc3339_ms(dt.fire_at), "2026-11-14T09:00:00.250Z");

    auto r2 = config::load_workflow_string(
        "name: d\ntriggers:\n  - type: date\n    date: \"2026-11-14T06:00:00-03:00\"\n"
        "jobs:\n  j:\n    steps:\n      - name: s\n        command: /bin/true\n");
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(villa::format_rfc3339_ms(
                  std::get<DateTrigger>(r2.triggers[0].spec).fire_at),
              "2026-11-14T09:00:00.000Z");
}

// ── Scheduler timer continuity ──────────────────────────────────────────

class VillaSchedulerTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock_.set_now(std::chrono::system_clock::time_point{
            std::chrono::hours(24 * 365 * 56)});
        clock_.set_steady(std::chrono::steady_clock::time_point{
            std::chrono::hours(1)});
        store_ = std::make_unique<persist::OccurrenceStore>(
            dir_.path() / "kairos.db");
    }

    void start(std::shared_ptr<const WorkflowRegistry> reg) {
        sched_ = std::make_unique<Scheduler>(SchedulerConfig{},
            Scheduler::Dependencies{
                .clock = &clock_,
                .registry = reg,
                .active_runs = &active_,
                .occurrences = store_.get(),
            });
        sched_->start(stop_.get_token(), [this](TriggerEvent e) {
            std::lock_guard lock(mu_);
            fired_.push_back(std::move(e));
            return true;
        });
        ASSERT_TRUE(applied(reg));
    }

    bool applied(const std::shared_ptr<const WorkflowRegistry>& reg) {
        return wait_for([&] {
            auto s = sched_->integration_snapshot();
            return s.applied && s.applied_digest == reg->registry_digest();
        });
    }

    void reload(std::shared_ptr<const WorkflowRegistry> reg) {
        sched_->request_reload(reg);
        ASSERT_TRUE(applied(reg));
    }

    IntegrationTimer only_timer() {
        auto s = sched_->integration_snapshot();
        EXPECT_EQ(s.timers.size(), 1u);
        return s.timers.empty() ? IntegrationTimer{} : s.timers[0];
    }

    size_t fired_count() {
        std::lock_guard lock(mu_);
        return fired_.size();
    }

    void TearDown() override {
        stop_.request_stop();
        clock_.wake();
        if (sched_) sched_->stop();
    }

    kairos::testing::TempDir dir_;
    kairos::testing::FakeClock clock_;
    ActiveRunTracker active_;
    std::unique_ptr<persist::OccurrenceStore> store_;
    std::unique_ptr<Scheduler> sched_;
    std::stop_source stop_;
    std::mutex mu_;
    std::vector<TriggerEvent> fired_;
};

TEST_F(VillaSchedulerTest, IntervalUnchangedAcrossUnrelatedReloads) {
    auto s1 = make_wf(sched_name(1), "S1");
    auto other = make_wf("villa.manual.run_" + std::string(32, 'b'), "M");
    auto r1 = make_reg({s1}, {interval_for(s1, 60s)});
    start(r1);

    auto t0 = only_timer();
    EXPECT_EQ(t0.schedule_id, kSid);
    EXPECT_EQ(t0.schedule_generation, 1);
    EXPECT_EQ(t0.trigger_digest,
              villa::trigger_digest(IntervalTrigger{.interval = 60000ms}));
    EXPECT_EQ(t0.next_due_at,
              villa::format_rfc3339_ms(clock_.now() + 60s));

    // Add/remove an unrelated workflow repeatedly over 50 seconds.
    for (int i = 0; i < 5; ++i) {
        clock_.advance(5s);
        reload(make_reg({s1, other}, {interval_for(s1, 60s)}));
        auto t = only_timer();
        EXPECT_EQ(t.next_due_at, t0.next_due_at);
        EXPECT_EQ(t.timer_epoch, t0.timer_epoch);
        clock_.advance(5s);
        reload(r1);
        t = only_timer();
        EXPECT_EQ(t.next_due_at, t0.next_due_at);
        EXPECT_EQ(t.timer_epoch, t0.timer_epoch);
    }
    EXPECT_EQ(fired_count(), 0u);

    // Fires at the ORIGINAL deadline (t0 + 60s), not reload time + 60s.
    clock_.advance(10s);
    ASSERT_TRUE(wait_for([&] { return fired_count() == 1; }));

    // Occurrence persisted before delivery, identity in the payload.
    TriggerEvent evt;
    { std::lock_guard lock(mu_); evt = fired_[0]; }
    auto& p = std::get<ScheduleTickPayload>(evt.payload);
    EXPECT_EQ(p.scheduled_for, t0.next_due_at);
    EXPECT_EQ(p.registry_digest, r1->registry_digest());
    auto rec = store_->get(p.occurrence_id);
    ASSERT_TRUE(rec);
    EXPECT_EQ(rec->disposition, "emitted");
    EXPECT_EQ(rec->workflow_name, sched_name(1));
    EXPECT_EQ(rec->schedule_id, kSid);
    EXPECT_EQ(rec->scheduled_for, t0.next_due_at);
    EXPECT_FALSE(rec->engine_run_id);

    // Next deadline keeps the phase and the epoch.
    auto t1 = only_timer();
    EXPECT_EQ(t1.timer_epoch, t0.timer_epoch);
    EXPECT_EQ(t1.next_due_at,
              villa::format_rfc3339_ms(clock_.now() + 60s));
}

TEST_F(VillaSchedulerTest, GenerationChangeWithSameTriggerTransfersState) {
    auto g1 = make_wf(sched_name(1), "G1");
    auto g2 = make_wf(sched_name(2), "G2");
    start(make_reg({g1}, {interval_for(g1, 60s)}));
    auto t0 = only_timer();

    clock_.advance(30s);
    // Intermediate registry: both generations present → one timer, newest.
    reload(make_reg({g1, g2}, {interval_for(g1, 60s), interval_for(g2, 60s)}));
    auto t = only_timer();
    EXPECT_EQ(t.workflow, sched_name(2));
    EXPECT_EQ(t.schedule_generation, 2);
    EXPECT_EQ(t.timer_epoch, t0.timer_epoch);
    EXPECT_EQ(t.next_due_at, t0.next_due_at);

    reload(make_reg({g2}, {interval_for(g2, 60s)}));
    t = only_timer();
    EXPECT_EQ(t.timer_epoch, t0.timer_epoch);
    EXPECT_EQ(t.next_due_at, t0.next_due_at);

    clock_.advance(30s);
    ASSERT_TRUE(wait_for([&] { return fired_count() == 1; }));
    std::lock_guard lock(mu_);
    EXPECT_EQ(fired_[0].target_id, g2.workflow_id);
}

TEST_F(VillaSchedulerTest, ChangedTriggerStartsNewEpoch) {
    auto g1 = make_wf(sched_name(1), "G1");
    auto g2 = make_wf(sched_name(2), "G2");
    start(make_reg({g1}, {interval_for(g1, 60s)}));
    auto t0 = only_timer();
    clock_.advance(30s);
    reload(make_reg({g2}, {interval_for(g2, 120s)}));
    auto t = only_timer();
    EXPECT_NE(t.timer_epoch, t0.timer_epoch);
    EXPECT_NE(t.trigger_digest, t0.trigger_digest);
    EXPECT_EQ(t.next_due_at, villa::format_rfc3339_ms(clock_.now() + 120s));
}

TEST_F(VillaSchedulerTest, AbsentScheduleComingBackStartsNewEpoch) {
    auto g1 = make_wf(sched_name(1), "G1");
    auto other = make_wf("other", "O");
    auto r1 = make_reg({g1}, {interval_for(g1, 60s)});
    start(r1);
    auto t0 = only_timer();
    clock_.advance(10s);
    reload(make_reg({other}, {}));
    EXPECT_TRUE(sched_->integration_snapshot().timers.empty());
    clock_.advance(10s);
    reload(r1);
    auto t = only_timer();
    EXPECT_NE(t.timer_epoch, t0.timer_epoch);
    EXPECT_EQ(t.next_due_at, villa::format_rfc3339_ms(clock_.now() + 60s));
}

TEST_F(VillaSchedulerTest, EngineRestartStartsNewEpoch) {
    auto g1 = make_wf(sched_name(1), "G1");
    auto r1 = make_reg({g1}, {interval_for(g1, 60s)});
    start(r1);
    auto t0 = only_timer();
    stop_.request_stop();  // run() observes the external token
    clock_.wake();
    sched_->stop();
    sched_.reset();
    stop_ = std::stop_source{};
    start(r1);
    EXPECT_NE(only_timer().timer_epoch, t0.timer_epoch);
}

TEST_F(VillaSchedulerTest, NonVillaTimersMatchByTriggerIdAndDigest) {
    auto wf = make_wf("plain", "P");
    auto other = make_wf("other", "O");
    start(make_reg({wf}, {interval_for(wf, 60s)}));
    auto t0 = only_timer();
    EXPECT_EQ(t0.schedule_id, "");
    EXPECT_EQ(t0.schedule_generation, -1);
    clock_.advance(20s);
    reload(make_reg({wf, other}, {interval_for(wf, 60s)}));
    auto t = only_timer();
    EXPECT_EQ(t.timer_epoch, t0.timer_epoch);
    EXPECT_EQ(t.next_due_at, t0.next_due_at);
}

TEST_F(VillaSchedulerTest, OneShotConsumedStateSurvivesReload) {
    auto g1 = make_wf(sched_name(1), "G1");
    auto other = make_wf("other", "O");
    auto at = clock_.now() + 10s;
    auto r1 = make_reg({g1}, {date_for(g1, at)});
    start(r1);
    clock_.advance(11s);
    ASSERT_TRUE(wait_for([&] { return fired_count() == 1; }));
    EXPECT_TRUE(sched_->integration_snapshot().timers.empty());

    reload(make_reg({g1, other}, {date_for(g1, at)}));
    clock_.advance(1s);
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(fired_count(), 1u);
    EXPECT_TRUE(sched_->integration_snapshot().timers.empty());

    // Restart: already-recorded one-shot is not refired either.
    stop_.request_stop();  // run() observes the external token
    clock_.wake();
    sched_->stop();
    sched_.reset();
    stop_ = std::stop_source{};
    start(r1);
    clock_.advance(1s);
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(fired_count(), 1u);
}

TEST_F(VillaSchedulerTest, MaxInstancesSkipIsRecorded) {
    auto g1 = make_wf(sched_name(1), "G1");
    start(make_reg({g1}, {interval_for(g1, 60s)}));
    active_.increment(g1.workflow_id);  // A run of this target is active.
    clock_.advance(61s);
    ASSERT_TRUE(wait_for([&] {
        return store_->list_after_seq(0, 10).size() == 1;
    }));
    EXPECT_EQ(fired_count(), 0u);
    auto rows = store_->list_after_seq(0, 10);
    EXPECT_EQ(rows[0].disposition, "skipped");
    EXPECT_EQ(rows[0].workflow_name, sched_name(1));
}

// ── Occurrence store ────────────────────────────────────────────────────

TEST(VillaOccurrenceStore, CursorOrderingAndRestartRecovery) {
    kairos::testing::TempDir dir;
    auto db = dir.path() / "kairos.db";
    std::string started_id, pending_id;
    {
        persist::OccurrenceStore store(db);
        auto a = store.record({.workflow_name = sched_name(1),
                               .schedule_id = kSid, .trigger_id = "trg-1",
                               .registry_digest = "d1",
                               .scheduled_for = "2026-11-14T09:00:00.000Z",
                               .disposition = "emitted"});
        auto b = store.record({.workflow_name = sched_name(1),
                               .schedule_id = kSid, .trigger_id = "trg-1",
                               .registry_digest = "d1",
                               .scheduled_for = "2026-11-14T09:01:00.000Z",
                               .disposition = "emitted"});
        EXPECT_LT(a.seq, b.seq);
        EXPECT_NE(a.occurrence_id, b.occurrence_id);
        EXPECT_EQ(a.occurrence_id.rfind("occ_", 0), 0u);
        EXPECT_TRUE(store.attach_run(a.occurrence_id, "run-abc"));
        started_id = a.occurrence_id;
        pending_id = b.occurrence_id;
    }
    // "Daemon restart": a fresh store over the same file.
    persist::OccurrenceStore store(db);
    EXPECT_EQ(store.mark_unstarted_skipped(), 1);
    auto started = store.get(started_id);
    auto pending = store.get(pending_id);
    ASSERT_TRUE(started && pending);
    EXPECT_EQ(started->disposition, "emitted");
    EXPECT_EQ(*started->engine_run_id, "run-abc");
    EXPECT_EQ(pending->disposition, "skipped");
    EXPECT_FALSE(pending->engine_run_id);
    EXPECT_EQ(pending->occurrence_id, pending_id);  // identity preserved

    auto all = store.list_after_seq(0, 500);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].occurrence_id, started_id);
    auto after = store.list_after_seq(all[0].seq, 500);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].occurrence_id, pending_id);

    // Change feed: the skip is the newest revision.
    auto changes = store.list_after_rev(0, 500);
    ASSERT_FALSE(changes.empty());
    EXPECT_EQ(changes.back().occurrence_id, pending_id);

    EXPECT_TRUE(store.exists_for("whatever", kSid, "2026-11-14T09:00:00.000Z"));
    EXPECT_FALSE(store.exists_for("whatever", "", "2026-11-14T09:00:00.000Z"));
    EXPECT_EQ(store.prune(std::chrono::hours(1)), 0);
}

// ── Pipeline: run binding + trusted env ─────────────────────────────────

TEST(VillaPipeline, ScheduledRunGetsTrustedOccurrenceEnv) {
    kairos::testing::TempDir dir;
    persist::OccurrenceStore store(dir.path() / "kairos.db");
    kairos::testing::FakeClock clock;
    std::stop_source stop;

    std::mutex mu;
    std::vector<std::unordered_map<std::string, std::string>> envs;
    exec::RunnerPool pool(exec::RunnerPoolConfig{.worker_count = 1,
                                                 .queue_capacity = 8});
    pool.set_process_handle_factory([&](const exec::ProcessSpec& spec) {
        {
            std::lock_guard lock(mu);
            envs.push_back(spec.environment);
        }
        auto p = std::make_unique<kairos::testing::FakeProcessHandle>();
        p->set_exit_code(0);
        return p;
    });
    pool.start(stop.get_token());

    auto wf = make_wf(sched_name(3), "G3");
    // A workflow trying to spoof the trusted namespace is ignored.
    wf.jobs[0].steps[0].env["VILLA_KAIROS_OCCURRENCE_ID"] = "spoofed";
    wf.jobs[0].env["VILLA_KAIROS_CONFIG_HASH"] = "spoofed";
    auto reg = make_reg({wf}, {});
    TriggerBus bus(8);
    ActiveRunTracker active;
    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock, .trigger_bus = &bus, .runner_pool = &pool,
        .registry = reg, .active_runs = &active,
        .occurrences = &store,
    });
    EXPECT_EQ(pipeline.applied_registry_digest(), reg->registry_digest());

    auto rec = store.record({.workflow_name = wf.workflow_name,
                             .schedule_id = kSid, .trigger_id = "trg-x",
                             .registry_digest = reg->registry_digest(),
                             .scheduled_for = "2026-11-14T09:00:00.000Z",
                             .disposition = "emitted"});
    auto evt = TriggerEvent::make_schedule_tick(
        "trg-x", wf.workflow_id, TriggerEvent::TargetKind::Workflow,
        clock.now(), "corr-sched-1",
        ScheduleTickPayload{.trigger_type = "interval",
                            .occurrence_id = rec.occurrence_id,
                            .scheduled_for = rec.scheduled_for,
                            .registry_digest = rec.registry_digest});
    EXPECT_EQ(pipeline.process_event(evt, stop.get_token()), RunStatus::Success);

    // Manual run of the same workflow: no VILLA_KAIROS_* at all.
    EXPECT_EQ(pipeline.process_event(
                  TriggerEvent::make_manual_run(
                      wf.workflow_id, TriggerEvent::TargetKind::Workflow,
                      "corr-man-1", "http"),
                  stop.get_token()),
              RunStatus::Success);

    std::lock_guard lock(mu);
    ASSERT_EQ(envs.size(), 2u);
    auto& e = envs[0];
    EXPECT_EQ(e["VILLA_KAIROS_OCCURRENCE_ID"], rec.occurrence_id);
    EXPECT_EQ(e["VILLA_KAIROS_SCHEDULED_FOR"], "2026-11-14T09:00:00.000Z");
    EXPECT_EQ(e["VILLA_KAIROS_CONFIG_HASH"], reg->registry_digest());
    EXPECT_EQ(e["KAIROS_CORRELATION_ID"], "corr-sched-1");
    EXPECT_FALSE(e["KAIROS_RUN_ID"].empty());
    EXPECT_FALSE(e["KAIROS_JOB_ID"].empty());
    EXPECT_FALSE(e["KAIROS_STEP_ID"].empty());

    auto bound = store.get(rec.occurrence_id);
    ASSERT_TRUE(bound && bound->engine_run_id);
    EXPECT_EQ(*bound->engine_run_id, e["KAIROS_RUN_ID"]);
    EXPECT_EQ(bound->disposition, "emitted");

    for (const auto& [k, v] : envs[1]) {
        EXPECT_NE(k.rfind("VILLA_KAIROS_", 0), 0u) << k;
    }
    stop.request_stop();
    pool.shutdown();
}

TEST(VillaPipeline, UnresolvableScheduledTargetIsAbandoned) {
    kairos::testing::TempDir dir;
    persist::OccurrenceStore store(dir.path() / "kairos.db");
    kairos::testing::FakeClock clock;
    std::stop_source stop;
    TriggerBus bus(8);
    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock, .trigger_bus = &bus,
        .registry = make_reg({}, {}), .occurrences = &store,
    });
    auto rec = store.record({.workflow_name = "gone", .trigger_id = "t",
                             .registry_digest = "d",
                             .scheduled_for = "2026-11-14T09:00:00.000Z",
                             .disposition = "emitted"});
    auto evt = TriggerEvent::make_schedule_tick(
        "t", "wfl-gone", TriggerEvent::TargetKind::Workflow, clock.now(),
        "c", ScheduleTickPayload{.occurrence_id = rec.occurrence_id});
    EXPECT_EQ(pipeline.process_event(evt, stop.get_token()), RunStatus::Failure);
    EXPECT_EQ(store.get(rec.occurrence_id)->disposition, "abandoned");
}
