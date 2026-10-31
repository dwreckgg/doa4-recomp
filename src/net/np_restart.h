// DOA4 netplay phase 1 (Task 12): the restart into an online session and back out of it, and the folders a session
// uses. No SDK: the path helpers and the relaunch plan are pure; ClearFolder, WriteSaveFiles and Relaunch touch the
// disk / start a process (Windows). Host-tested by tests/host/np_restart_host_test.cpp.
//
// Folders (<U> = the player's user data folder, the one --user_data_root / doa4_user_data / the default names; never
// the redirected one):
//   <U>/netplay/handoff.d4h                         the hand-off the lobby writes before the restart (HandoffPath)
//   <U>/netplay/session                             the session's user data: cleared, then the host's save written into
//                                                   it; the runtime's user_data_root in a session (SessionUserData)
//   <U>/netplay/sessions/<yyyymmdd-hhmmss>_<role>   what one session writes (np_log, rec.d4r, digests, events.txt,
//                                                   ud_start/); with DOA4_NP_TEST_DIR=<T>: <T>/session (SessionDir)
#pragma once
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "np_handoff.h"

namespace doa4::np::restart {

std::filesystem::path HandoffPath(const std::filesystem::path& user_data);      // <U>/netplay/handoff.d4h
std::filesystem::path SessionUserData(const std::filesystem::path& user_data);  // <U>/netplay/session
// <T>/session when test_dir is not empty, else <U>/netplay/sessions/<yyyymmdd-hhmmss>_<host|joiner> (local time of now)
std::filesystem::path SessionDir(const std::filesystem::path& user_data, const std::filesystem::path& test_dir, bool host,
                                 std::time_t now);

// Makes `dir` an existing, empty folder. Refuses (false, *error) when `dir` itself is a reparse point (a junction or a
// symbolic link): the session folder must be a real folder. Inside it, a reparse point is removed as a LINK (the
// link itself, never what it points to: nothing is followed or deleted through it); real folders are emptied and
// removed depth first. A file that cannot be removed fails the call (the session must not start on a half-cleared
// folder).
bool ClearFolder(const std::filesystem::path& dir, std::string* error);

// Writes each file below `dir` (parent folders created). A name handoff::NameOk refuses is not written (and the call
// returns false, naming it in *error); the others are still written. Nothing is ever written outside `dir`: NameOk
// allows only plain components below the save prefix. A write that fails returns false.
bool WriteSaveFiles(const std::filesystem::path& dir, const std::vector<handoff::File>& files, std::string* error);

// What Relaunch does, as data (pure; host-tested): the environment the child gets (set just before CreateProcessW in
// this process, restored right after: the child alone has them) and the show state it starts with.
struct EnvVar {
  std::wstring name;
  std::wstring value;  // empty = removed in the child
};
struct RelaunchPlan {
  std::wstring command_line;  // this process's own command line, unchanged
  std::vector<EnvVar> env;    // DOA4_WAIT_PID, DOA4_NP_HANDOFF (set or removed), DOA4_NP_AFTER_SESSION (set or removed)
  bool use_show = false;      // STARTUPINFO dwFlags STARTF_USESHOWWINDOW
  unsigned short show = 0;    // wShowWindow
};
// parent_use_show / parent_show: this process's own STARTUPINFO (GetStartupInfoW): a game started hidden or minimized
// by the harness (np_run / np_pair: STARTF_USESHOWWINDOW + SW_SHOWMINNOACTIVE) relaunches the same way (Task 10 review
// I4); a normal start (no flag) relaunches normally.
RelaunchPlan PlanRelaunch(const std::wstring& command_line, unsigned long parent_pid, const std::filesystem::path* handoff,
                          bool after_session, bool parent_use_show, unsigned short parent_show);

// Starts this exe again with the same command line, as PlanRelaunch says (like MenuDialog::Restart, doa4_menu.cpp:
// 677-700). The caller then quits. False (and *error) when the process could not be created.
bool Relaunch(const std::filesystem::path* handoff, bool after_session, std::string* error);

}  // namespace doa4::np::restart
