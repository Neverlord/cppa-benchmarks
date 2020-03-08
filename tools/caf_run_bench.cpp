#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <chrono>
#include <fstream>
#include <iostream>

#include "caf/all.hpp"

#ifdef __APPLE__
# include <mach/mach.h>
# include <mach/message.h>
# include <mach/task_info.h>
# include <mach/kern_return.h>
#endif

#ifdef CAF_BEGIN_TYPE_ID_BLOCK

CAF_BEGIN_TYPE_ID_BLOCK(run_bench, first_custom_type_id)

  CAF_ADD_ATOM(run_bench, go_atom);
  CAF_ADD_ATOM(run_bench, poll_atom);

CAF_END_TYPE_ID_BLOCK(run_bench)

#else

using go_atom = caf::atom_constant<caf::atom("go")>;
using poll_atom = caf::atom_constant<caf::atom("poll")>;
using timeout_atom = caf::atom_constant<caf::atom("timeout")>;
constexpr go_atom go_atom_v = go_atom::value;
constexpr poll_atom poll_atom_v = poll_atom::value;
constexpr timeout_atom timeout_atom_v = timeout_atom::value;

#endif

using namespace caf;

using std::string;

namespace {

decltype(std::chrono::system_clock::now()) s_start;

} // namespace

#ifdef __APPLE__
template <class OutStream>
bool print_rss(OutStream& out, std::string&, const string&, pid_t child) {
  task_t child_task;
  if (task_for_pid(mach_task_self(), child, &child_task) != KERN_SUCCESS) {
    return false;
  }
  task_basic_info_data_t basic_info;
  mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
  if (task_info(child_task, TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&basic_info), &count) != KERN_SUCCESS) {
    return false;
  }
  auto rss = static_cast<unsigned long long>(basic_info.resident_size);
  auto rss_kb = rss / 1024;
  // type is mach_vm_size_t
  out << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - s_start).count()
      << " "
      << rss_kb << std::endl;
  return true;
}
#elif defined(__linux__)
template <class OutStream>
bool print_rss(OutStream& out, string& line, const string& proc_file, pid_t) {
  std::ifstream statfile(proc_file);
  while (std::getline(statfile, line)) {
    if (line.compare(0, 6, "VmRSS:") == 0) {
      auto first = line.c_str() + 6; // skip "VmRSS:"
      auto rss = std::strtoll(first, NULL, 10);
      if (line.compare(line.size() - 2, 2, "kB") != 0) {
        std::cerr << "VmRSS is *NOT* in kB" << std::endl;
      }
      out << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - s_start).count()
          << " "
          << rss << std::endl;
      return true;
    }
  }
  return false;
}
#else
# error OS not supported
#endif

void watchdog(blocking_actor* self, int max_runtime) {
  pid_t child;
  self->receive(
    [&](go_atom, pid_t child_pid) {
      child = child_pid;
    }
  );
  self->delayed_send(self, std::chrono::seconds(max_runtime), timeout_atom_v);
  self->receive(
    [=](timeout_atom) {
      kill(child, 9);
    },
    [&](const exit_msg& msg) {
      if (msg.reason) {
       self->fail_state(std::move(msg.reason));
      }
    }
  );
}

void memrecord(blocking_actor* self, int poll_interval, std::ostream* out_ptr) {
  auto& out = *out_ptr;
  string fname = "/proc/";
  string line_buf;
  pid_t child;
  self->receive(
    [&](go_atom, pid_t child_pid) {
      child = child_pid;
    }
  );
  fname += std::to_string(child);
  fname += "/status";
  self->send(self, poll_atom_v);
  bool running = true;
  self->receive_while(running)(
    [&](poll_atom) {
      self->delayed_send(self, std::chrono::milliseconds(poll_interval),
                         poll_atom_v);
      print_rss(out , line_buf, fname, child);
    },
    [&](const exit_msg& msg) {
      if (msg.reason) {
        self->fail_state(std::move(msg.reason));
        running = false;
      }
    });
}

namespace {

class my_config : public actor_system_config {
public:
  int userid = 1000;
  int max_runtime = 3600;
  int mem_poll_interval = 50;
  string runtime_out_fname;
  string mem_out_fname;
  string bench;

  my_config() {
    opt_group{custom_options_, "global"}
      .add(userid, "uid, u", "set user id")
      .add(max_runtime, "max-runtime", "set maximum runtime (in sec)")
      .add(mem_poll_interval, "mem-poll-interval",
           "set memory poll intervall (in ms)")
      .add(runtime_out_fname, "runtime-out", "set runtime filename")
      .add(mem_out_fname, "mem-out", "set memory filename")
      .add(bench, "bench", "set executable of the benchmark + plus args");
  }
};

void init_fstream(const string& fname, std::fstream& fs) {
  if (!fname.empty()) {
    fs.open(fname, std::ios_base::out | std::ios_base::app);
    if (!fs) {
      std::cerr << "unable to open file for runtime output: " << fname
                << std::endl;
      exit(1);
    }
  }
}

int caf_main(actor_system& system, const my_config& cfg) {
#ifdef CAF_BEGIN_TYPE_ID_BLOCK
  init_global_meta_objects<run_bench_type_ids>();
#endif
  std::fstream runtime_out;
  std::fstream mem_out;
  init_fstream(cfg.runtime_out_fname, runtime_out);
  init_fstream(cfg.mem_out_fname, mem_out);
  std::ostringstream mem_out_buf;
  // start background workers
  auto dog = system.spawn<detached>(watchdog, cfg.max_runtime);
  actor mem_rec;
  if (mem_out)
    mem_rec = system.spawn<detached>(memrecord, cfg.mem_poll_interval, &mem_out_buf);
  std::cout << "fork into " << cfg.bench << std::endl;
  pid_t child_pid = fork();
  if (child_pid < 0) {
    std::cerr << "fork failed" << std::endl,
    abort();
  }
  s_start = std::chrono::system_clock::now();
  if (child_pid == 0) {
    if (setuid(static_cast<uid_t>(cfg.userid)) != 0) {
      std::cerr << "could not set userid to " << cfg.userid << std::endl;
      exit(1);
    }
    // make sure $HOME is set properly (evaluated by Erlang)
    auto pw = getpwuid(static_cast<uid_t>(cfg.userid));
    std::string env_cmd = "HOME=";
    env_cmd += pw->pw_dir;
    if (putenv(&env_cmd[0]) != 0) {
      std::cerr << "could net set HOME to " << pw->pw_dir << std::endl;
      exit(1);
    }
    std::vector<char*> arr;
    arr.emplace_back(const_cast<char*>(cfg.bench.c_str()));
    for (size_t i = 0; i < cfg.remainder.size(); ++i) {
      arr.emplace_back(const_cast<char *>(cfg.remainder[i].c_str()));
    }
    arr.emplace_back(nullptr);
    execv(cfg.bench.c_str(), arr.data());
    // should be unreachable
    std::cerr << "execv failed" << std::endl;
    abort();
  }
  auto msg = make_message(go_atom_v, child_pid);
  anon_send(dog, msg);
  if (mem_out)
    anon_send(mem_rec, msg);
  int child_exit_status = 0;
  wait(&child_exit_status);
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - s_start);
  anon_send_exit(dog, exit_reason::user_shutdown);
  if (mem_out)
    anon_send_exit(mem_rec, exit_reason::user_shutdown);
  std::cout << "exit status: " << child_exit_status << std::endl;
  std::cout << "program did run for " << duration.count() << "ms" << std::endl;
  system.await_all_actors_done();
  if (child_exit_status == 0) {
    if (runtime_out)
      runtime_out << duration.count() << std::endl;
    if (mem_out)
      mem_out << mem_out_buf.str() << std::flush;
  }
  return child_exit_status;
}

} // namespace <anonymous>

CAF_MAIN();
