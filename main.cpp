#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

using namespace std;

#define RESET "\033[0m"
#define DGREEN "\033[36m"
#define GREEN "\033[32m"
#define PROMPT "\033[32m$ \033[0m"
#define RED "\033[31m"

struct Command {
  vector<string> args;

  bool redirect_stdout = false;
  string stdout_file;
  bool append_stdout = false;

  bool redirect_stderr = false;
  string stderr_file;
  bool append_stderr = false;
};

void disableRawMode(struct termios &org_ter) {
  tcsetattr(STDIN_FILENO, TCSANOW, &org_ter);
}

void enableRawMode(struct termios &org_ter) {
  struct termios new_ter;
  tcgetattr(STDIN_FILENO, &org_ter);
  new_ter = org_ter;
  new_ter.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &new_ter);
}

void apply_redirection(const Command &cmd, int &saved_stdout, int &saved_stderr,
                       bool &did_redirect_stdout, bool &did_redirect_stderr) {
  did_redirect_stdout = false;
  did_redirect_stderr = false;

  if (cmd.redirect_stdout && !cmd.stdout_file.empty()) {
    saved_stdout = dup(STDOUT_FILENO);
    int flags = O_WRONLY | O_CREAT | (cmd.append_stdout ? O_APPEND : O_TRUNC);
    int fd = open(cmd.stdout_file.c_str(), flags, 0644);
    if (fd == -1) {
      perror(("open stdout file: " + cmd.stdout_file).c_str());
    } else {
      dup2(fd, STDOUT_FILENO);
      close(fd);
      did_redirect_stdout = true;
    }
  }

  if (cmd.redirect_stderr && !cmd.stderr_file.empty()) {
    saved_stderr = dup(STDERR_FILENO);
    int flags = O_WRONLY | O_CREAT | (cmd.append_stderr ? O_APPEND : O_TRUNC);
    int fd = open(cmd.stderr_file.c_str(), flags, 0644);
    if (fd == -1) {
      perror(("open stderr file: " + cmd.stderr_file).c_str());
    } else {
      dup2(fd, STDERR_FILENO);
      close(fd);
      did_redirect_stderr = true;
    }
  }
}

void restore_io(int saved_stdout, int saved_stderr, bool did_redirect_stdout,
                bool did_redirect_stderr) {
  if (did_redirect_stdout) {
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
  }
  if (did_redirect_stderr) {
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
  }
}

vector<string> get_matches(const string &prefix) {
  const char *env_path = getenv("PATH");
  string path;
  stringstream ss(env_path ? env_path : "");
  vector<string> matches;
  while (getline(ss, path, ':')) {
    if (filesystem::exists(path)) {
      for (const auto &entry : filesystem::directory_iterator(path)) {
        string filename = entry.path().filename().string();
        if (filename.rfind(prefix, 0) == 0 &&
            access(entry.path().c_str(), X_OK) == 0)
          matches.push_back(filename);
      }
    }
  }
  return matches;
}

string lcp_(const vector<string> &strings) {
  if (strings.empty())
    return "";
  string prefix = strings[0];
  for (size_t i = 1; i < strings.size(); i++) {
    size_t j = 0;
    while (j < prefix.size() && j < strings[i].size() &&
           prefix[j] == strings[i][j]) {
      j++;
    }
    prefix = prefix.substr(0, j);
    if (prefix.empty())
      break;
  }
  return prefix;
}

string read_input() {
  string input;
  size_t cursor = 0;
  struct termios org_ter;
  enableRawMode(org_ter);
  while (true) {
    char ch = getchar();
    if (ch == '\033') { // Escape sequence for arrow keys
      char nextch = getchar();
      if (nextch == '[') {
        char nextch2 = getchar();
        switch (nextch2) {
        case 'C': // right arrow
          if (cursor < input.size()) {
            cursor++;
            cout << "\033[C" << flush;
          }
          break;
        case 'D': // left arrow
          if (cursor > 0) {
            cursor--;
            cout << "\033[D" << flush;
          }
          break;
        default:
          continue;
        }
      }
    } else if (ch == '\n') {
      cout << endl;
      break;
    } else if (ch == '\t') {
      if (input.empty())
        continue;
      auto matches = get_matches(input);
      if (!matches.empty()) {
        if (matches.size() == 1) {
          input = matches[0] + " ";
          cursor = input.length();
          cout << "\r" << PROMPT << input << flush;
        } else {
          string lcp = lcp_(matches);
          if (lcp != input) {
            input = lcp;
            cursor = input.length();
            cout << "\r" << PROMPT << input << flush;
          } else {
            for (const auto &match : matches)
              cout << match << " ";
            cout << endl << PROMPT << flush;
          }
        }
      }
    } else if (ch == 127 || ch == 8) { // Backspace
      if (cursor > 0) {
        input.erase(input.begin() + cursor - 1);
        cursor--;
        cout << "\r" << PROMPT << input << ' ';
        cout << "\r" << flush;
        cout << "\033[" << (2 + cursor) << "C" << flush;
        size_t diff = input.size() - cursor;
        if (diff > 0)
          cout << "\033[" << diff << "D" << flush;
      }
    } else if (isprint(ch)) {
      input.insert(input.begin() + cursor, ch);
      cursor++;
      cout << "\r" << PROMPT << input;
      size_t diff = input.size() - cursor;
      if (diff > 0) {
        cout << "\033[" << diff << "D" << flush;
      }
    }
  }
  disableRawMode(org_ter);
  return input;
}

Command parse_cmd(const string &input) {
  vector<string> args;
  string current;
  bool in_quotes = false;
  char quote_char = '\0';

  for (char ch : input) {
    if (ch == '\'' || ch == '\"') {
      if (in_quotes && ch == quote_char)
        in_quotes = false;
      else if (!in_quotes) {
        in_quotes = true;
        quote_char = ch;
      } else
        current += ch;
    } else if (isspace(ch) && !in_quotes) {
      if (!current.empty()) {
        args.push_back(current);
        current.clear();
      }
    } else
      current += ch;
  }
  if (!current.empty())
    args.push_back(current);

  Command cmd;

  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == ">" || args[i] == "1>") {
      if (i + 1 < args.size()) {
        cmd.stdout_file = args[i + 1];
        cmd.redirect_stdout = true;
        cmd.append_stdout = false;
        i++;
      }
    } else if (args[i] == ">>" || args[i] == "1>>") {
      if (i + 1 < args.size()) {
        cmd.stdout_file = args[i + 1];
        cmd.redirect_stdout = true;
        cmd.append_stdout = true;
        i++;
      }
    } else if (args[i] == "2>") {
      if (i + 1 < args.size()) {
        cmd.stderr_file = args[i + 1];
        cmd.redirect_stderr = true;
        cmd.append_stderr = false;
        i++;
      }
    } else if (args[i] == "2>>") {
      if (i + 1 < args.size()) {
        cmd.stderr_file = args[i + 1];
        cmd.redirect_stderr = true;
        cmd.append_stderr = true;
        i++;
      }
    } else {
      cmd.args.push_back(args[i]);
    }
  }

  return cmd;
}

bool run_builtin(const Command &cmd) {
  if (cmd.args.empty())
    return false;

  int saved_stdout = -1, saved_stderr = -1;
  bool did_redirect_stdout = false, did_redirect_stderr = false;
  apply_redirection(cmd, saved_stdout, saved_stderr, did_redirect_stdout,
                    did_redirect_stderr);

  auto restore_io_lambda = [&]() {
    restore_io(saved_stdout, saved_stderr, did_redirect_stdout,
               did_redirect_stderr);
  };

  const char *home = getenv("HOME");
  const char *env_path = getenv("PATH");
  const vector<string> builtins = {"echo",  "exit", "pwd",   "cd",  "c",
                                   "clear", "type", "which", "kill"};

  const string &cmd_name = cmd.args[0];

  if (cmd_name == "exit") {
    restore_io_lambda();
    exit(0);
  } else if (cmd_name == "cd") {
    string cdpath;
    if (cmd.args.size() < 2 || cmd.args[1].empty() || cmd.args[1] == "~") {
      cdpath = home ? string(home) : "/";
    } else if (cmd.args[1] == ".")
      cdpath = filesystem::current_path();
    else if (cmd.args[1] == "..")
      cdpath = filesystem::current_path().parent_path();
    else
      cdpath = filesystem::absolute(cmd.args[1]);

    if (chdir(cdpath.c_str()) != 0) {
      perror("cd");
    }
    restore_io_lambda();
    return true;
  } else if (cmd_name == "c" || cmd_name == "clear") {
    cout << "\033[2J\033[H" << flush;
    restore_io_lambda();
    return true;
  } else if (cmd_name == "pwd") {
    cout << filesystem::current_path().string() << endl;
    restore_io_lambda();
    return true;
  } else if (cmd_name == "type" || cmd_name == "which") {
    if (cmd.args.size() < 2) {
      cout << "[Usage]: " << cmd_name << " <Command>\n";
      restore_io_lambda();
      return true;
    }
    const string &query = cmd.args[1];
    if (find(builtins.begin(), builtins.end(), query) != builtins.end()) {
      cout << GREEN << query << RESET << DGREEN << ": is a shell builtin\n"
           << RESET;
    } else {
      stringstream ss(env_path ? env_path : "");
      string path;
      bool found = false;
      while (getline(ss, path, ':')) {
        string fullpath = path + '/' + query;
        if (filesystem::exists(fullpath) &&
            access(fullpath.c_str(), X_OK) == 0) {
          cout << query << ": is " << fullpath << endl;
          found = true;
          break;
        }
      }
      if (!found)
        cerr << query << ": not found\n";
    }
    restore_io_lambda();
    return true;
  } else if (cmd_name == "kill") {
    if (cmd.args.size() < 2) {
      cout << "[Usage]: kill <pid> <signal>\n";
      restore_io_lambda();
      return true;
    }
    try {
      pid_t pid = stoi(cmd.args[1]);
      int sig = SIGTERM;
      if (cmd.args.size() == 3)
        sig = stoi(cmd.args[2]);
      if (kill(pid, sig) != 0)
        perror(("kill " + to_string(pid)).c_str());
    } catch (const exception &e) {
      cerr << "kill: invalid argument " << e.what() << "\n";
    }
    restore_io_lambda();
    return true;
  } else if (cmd_name == "echo") {
    for (size_t i = 1; i < cmd.args.size(); i++) {
      cout << cmd.args[i];
      if (i != cmd.args.size() - 1)
        cout << " ";
    }
    cout << "\n";
    restore_io_lambda();
    return true;
  }

  restore_io_lambda();
  return false;
}

void exe_extr(const Command &cmd) {
  pid_t pid = fork();
  if (pid == 0) {
    // Child
    if (cmd.redirect_stdout && !cmd.stdout_file.empty()) {
      int flags = O_WRONLY | O_CREAT | (cmd.append_stdout ? O_APPEND : O_TRUNC);
      int fd = open(cmd.stdout_file.c_str(), flags, 0644);
      if (fd == -1) {
        perror(("cannot open file: " + cmd.stdout_file).c_str());
        exit(1);
      }
      dup2(fd, STDOUT_FILENO);
      close(fd);
    }
    if (cmd.redirect_stderr && !cmd.stderr_file.empty()) {
      int flags = O_WRONLY | O_CREAT | (cmd.append_stderr ? O_APPEND : O_TRUNC);
      int fd = open(cmd.stderr_file.c_str(), flags, 0644);
      if (fd == -1) {
        perror(("cannot open file: " + cmd.stderr_file).c_str());
        exit(1);
      }
      dup2(fd, STDERR_FILENO);
      close(fd);
    }

    vector<char *> argv;
    for (const auto &s : cmd.args)
      argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);

    execvp(argv[0], argv.data());
    cerr << RED << "fero: command not found: " << argv[0] << RESET << endl;
    exit(1);
  } else if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
  } else {
    perror("fork");
  }
}

int main() {
  cout << "\033[2J\033[H" << flush; 
  while (true) {
    string cwd = filesystem::current_path();
    cout << "\033[36m" << cwd << "\033[0m" << "\n" << PROMPT << flush;
    string input = read_input();
    if (input.empty())
      continue;

    Command cmd = parse_cmd(input);
    if (cmd.args.empty())
      continue;

    if (run_builtin(cmd))
      continue;

    exe_extr(cmd);
  }

  return 0;
}
