#include <cctype>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <termios.h>
#include <vector>

using namespace std;


#define RESET "\033[0m"
#define DGREEN "\033[36m"
#define GREEN  "\033[32m"
#define PROMPT "\033[32m$ \033[0m"
#define RED "\033[31m"


struct Command {
  vector<string> args;
  string stdout_file;
  string stderr_file;
  bool append_stdout = true;
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

// void render_input(const string& input, size_t cursor) {
//     const int PROMPT_WIDTH = 2;
//     cout << "\r" << PROMPT << input << ' ' << flush;
//     cout << "\r" << flush;
//     cout << "\033[" << (PROMPT_WIDTH + cursor) << "C" << flush;
// }


vector<string> get_matches(const string prefix) {
  const char *env_path = getenv("PATH");
  string path;
  stringstream ss(env_path);
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
  return vector<string>(matches.begin(), matches.end());
}

string lcp_(const vector<string> &strings) {
  if (strings.empty())
    return "";
  string prefix = strings[0];
  for (size_t i{1}; i < strings.size(); i++) {
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
    if(ch == '\033'){
      char nextch = getchar();
      if(nextch == '['){
        char nextch2 = getchar();
         switch(nextch2) {
            case 'C': if(cursor < input.size()){ cursor++; cout << "\033[C" << flush; }  break;
            case 'D': if(cursor > 0) { cursor--; cout << "\033[D" << flush; }  break;
            default: continue;
         } continue;
      } 
    }
    else if (ch == '\n') {
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
    } else if (ch == 127 || ch == 8) {
      if (cursor > 0){ 
        input.erase(input.begin()+ cursor - 1);
        cursor--;
        cout << "\r" << PROMPT << input << ' ';
        cout << "\r" << flush;
        /* '2' PROMPT WIDTH TOO LAZY TO MAKE A MACRO */
        cout << "\033[" << (2 + cursor) << "C" << flush;
        size_t diff = input.size() - cursor;
        if(diff > 0) cout << "\033[" << diff << "D" << flush;
      }
    } else if (isprint(ch)) {
      input.insert(input.begin() + cursor, ch);
      cursor++;
      cout << "\r" << PROMPT << input;
      size_t diff = input.size() - cursor;
      if(diff > 0){
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
  bool in_qoutes = false;
  char qoute_char = '\0';

  for (char ch : input) {
    if (ch == '\'' || ch == '\"') {
      if (in_qoutes && ch == qoute_char)
        in_qoutes = false;
      else if (!in_qoutes) {
        in_qoutes = true;
        qoute_char = ch;
      } else
        current += ch;
    } else if (isspace(ch) && !in_qoutes) {
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
  for(size_t i{0}; i < input.size(); i++){
    if(args[i] == ">" || args[i] == "1>"){
      if(i+1 < args.size()){
        cmd.stdout_file = args[i+1];
        cmd.append_stdout = false;
        i++;
      }
    } else if(args[i] == ">>" || args[i] == "1>>"){
      if(i+1 < args.size()){
        cmd.stdout_file = args[i+1];
        cmd.append_stdout = true;
        i++;
      }
    } else if(args[i] == "2>"){
      if(i+1 > args.size()){
        cmd.stderr_file = args[i+1];
        cmd.append_stderr = false;
        i++;
      }
    } else if(args[i] == "2>>"){
      if(i+1 < args.size()){
        cmd.stderr_file = args[i+1];
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
  const char *home = getenv("HOME");
  const char *env_path = getenv("PATH");
  // const string path = filesystem::absolute(args[1]);
  const vector<string> builtins = {"echo", "exit", "pwd", "cd", "c", "clear", "type", "which", "kill"};
  if (cmd.args[0] == "exit")
    exit(0);
  else if (cmd.args[0] == "cd") {
    string cdpath;
    if (cmd.args.size() < 2 || cmd.args[1].empty() || cmd.args[1] == "~") {
      // const char* home = getenv("HOME");
      cdpath = home ? string(home) : "/";
    } else if (cmd.args[1] == ".")
      cdpath = filesystem::current_path();
    else if (cmd.args[1] == "..")
      cdpath = filesystem::current_path().parent_path();
    else {
      cdpath = filesystem::absolute(cmd.args[1]); // notice
    }
    if (chdir(cdpath.c_str()) != 0) {
      perror("cd");
    }
    return true;
  } else if (cmd.args[0] == "c" || cmd.args[0] == "clear") {
    cout << "\033[2J\033[H" << flush; // pORtAbiLItY
    return true;
  } else if (cmd.args[0] == "pwd") {
    cout << filesystem::current_path().string() << endl;
    return true;
  } else if (cmd.args[0] == "type" || cmd.args[0] == "which"){
    if(cmd.args.size() < 2) { cout << "[Usage]: " << cmd.args[0] << " <Command>\n"; return true; }
    for(const auto& bltin : builtins) {
      if(bltin == cmd.args[1]) cout << GREEN << cmd.args[1] << RESET << DGREEN << ": is a shell builtin\n" << RESET;
      else {
        stringstream ss(env_path);
        string path;
        bool dsPathExists = false;
        while(getline(ss, path, ':')){
          string fullpath = path + '/' + cmd.args[1];
          if (filesystem::exists(fullpath) && access(fullpath.c_str(), X_OK) == 0) {
             cout << cmd.args[1] << ": is" << fullpath << endl;
             dsPathExists = true;
             break;
          }
        }
        if (!dsPathExists) cerr << cmd.args[1] << ": not found\n";
      }
    }  
    return true;
    }
  else if(cmd.args[0] == "kill"){

     if(cmd.args.size() < 2) { cout << "[Usage]: kill <pid> <signal>\n"; return true; }
     try{
     pid_t pid = stoi(cmd.args[1]);
     int sig = SIGTERM;
     if(cmd.args.size() == 3) sig = stoi(cmd.args[2]);
     if(kill(pid, sig) != 0) perror(("kill " + to_string(pid)).c_str());
     } catch(const exception& e){
       cerr << "kill: invalud argument " << e.what() << "\n";
     }
      
  return true;
  }
  else if(args[0] == "echo"){
    for(size_t i{1}; i < args.size(); i++) {
       cout << args[i];
       if(i != args.size() - 1) cout << " ";
    }
      cout << "\n";
      return true;
    }

    return false;
}

void exe_extr(const vector<string> &args) {
  pid_t pid = fork();
  if (pid == 0) {
    std::vector<char *> argv;
    for (const auto &s : args)
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
  cout << "\033[2J\033[H" << flush; // pORtAbiLItY
  while (true) {
    string cwd = filesystem::current_path();
    cout << "\033[36m" << cwd << "\033[0m" << "\n\033[32m$ \033[0m" << flush;
    string input = read_input();
    if (input.empty())
      continue;

    Command cmd = parse_cmd(input);
    if (cmd.args.empty())
      continue;

    if (run_builtin(cmd)) {
      continue;
    }
    exe_extr(cmd);
  }

  return 0;
}
