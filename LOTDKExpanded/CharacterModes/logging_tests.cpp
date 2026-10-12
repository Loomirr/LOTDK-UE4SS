#include <cassert>
#include <iostream>
#include "../VerboseLog.hpp"
struct {bool VerboseLogging=false;} SuitSettings;
enum class LogLevel {Verbose};
int sent=0;
struct Output {template<LogLevel,typename... Args> static void send(Args&&...){++sent;}};
int main(){int evaluated=0;
 for(int i=0;i<100000;++i)suit_vlog("disabled",++evaluated);
 assert(evaluated==0 && sent==0);
 SuitSettings.VerboseLogging=true;suit_vlog("enabled",++evaluated);
 assert(evaluated==1 && sent==1);
 if(false)suit_vlog("skip",++evaluated);else ++evaluated;
 assert(evaluated==2 && sent==1);
 std::cout<<"PASS production verbose-log gate skips all argument evaluation when disabled; enabled logging preserved\n";
}
