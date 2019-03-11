#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#include "util.h"

#include "chainparamsbase.h"
#include "fs.h"
#include "random.h"
#include "serialize.h"
#include "utilstrencodings.h"
#include "utiltime.h"

#include "wallet/wallet.h"
#include "wallet/telemetry_wallet.h"

#include <stdarg.h>
#include <queue>

#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/algorithm/string/predicate.hpp> // for startswith() and endswith()
#include <boost/program_options/detail/config_file.hpp>
#include <boost/thread.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/conf.h>

#define TELEMETRY_FILE "telemetry.dat"

static const char* telemerty_blacklist_arr[] = {"DebugMessageHandler","InitParameterInteraction","","AppInitMain","InitLogging"};
std::vector<std::string> telemerty_blacklist (telemerty_blacklist_arr, telemerty_blacklist_arr + sizeof(telemerty_blacklist_arr)/sizeof(telemerty_blacklist_arr[0]));

std::queue<std::string> telemetryQ;
std::mutex telemetryMutex;

std::string telemetry_adder(const char* format) {
	std::string str = boost::lexical_cast<std::string>(format);
	return str;
}

bool IsTelemetryBlacklisted(std::string func_name) {
	for(unsigned int i = 0; i < telemerty_blacklist.size(); i++) {
		if(func_name == telemerty_blacklist[i])
			return true;
	}
	return false;
}

void TelemetrySetBlacklisted(std::string blacklist) {
	std::stringstream ss(blacklist);
	telemerty_blacklist.clear();
	while( ss.good() ) {
	    std::string substr;
	    getline( ss, substr, ',' );
	    telemerty_blacklist.push_back( substr );
	}
}

std::string ReadTelemetryLogsSync() {
	std::lock_guard<std::mutex> lock(telemetryMutex);
	fs::path telemetry_path = GetDataDir() / TELEMETRY_FILE;
	std::string res = "";
	std::string line;
	std::ifstream tfile (telemetry_path.string());
	if (tfile.is_open()) {
		while ( getline (tfile,line) ) {
			res += line;
		}
		tfile.close();
	}
	return res;
}

void SaveTelemetryLogs(std::string logs, bool append) {
	try {
		fs::path telemetry_path = GetDataDir() / TELEMETRY_FILE;
		std::ofstream tfile;
		tfile.open (telemetry_path.string(), std::ios::out | ((append) ? std::ios::app : std::ios::trunc));
		if (tfile.is_open()) {
	  		tfile << logs;
	  		tfile.close();
		}
	}
	catch(const std::exception& e) {
                 std::cout << " a standard exception was caught, with message '" << e.what() << "'\n";
        }
}

void SaveTelemetryLogsSync(std::string logs, bool append) {
	std::lock_guard<std::mutex> lock(telemetryMutex);
	SaveTelemetryLogs(logs, append);
}

void TelemetryQSend(std::string msg) {
	if(msg == "")
		return;
	std::lock_guard<std::mutex> lock(telemetryMutex);
	SaveTelemetryLogs(msg, true);
	telemetryQ.push(std::move(msg));
}

std::string TelemetryQGet() {
	std::string ret = "";
	std::unique_lock<std::mutex> lock(telemetryMutex);
	while(!telemetryQ.empty()) {
		ret += telemetryQ.front();
		telemetryQ.pop();
	}
	return ret;
}

void SendToTelemetry(const std::string &str, const std::string &params) {
	std::string func_name = "";
	int64_t time = GetTimeMicros();
	int index = str.find(":");
	if(index != -1) {
		func_name = str.substr(0, index);
	}

	if(IsTelemetryBlacklisted(func_name))
		return;

	std::string log_json = "{\"func_name\":\"" + func_name + "\"," +
							"\"time\":" + boost::lexical_cast<std::string>(time) + "," +
							"\"msg\":\"" + str + "\"," +
							"\"format\":\"" + params + "\"},";

	TelemetryQSend(log_json);
}
