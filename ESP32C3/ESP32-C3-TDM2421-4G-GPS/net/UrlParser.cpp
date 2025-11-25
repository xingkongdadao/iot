#include "UrlParser.h"

bool parseUrl(const String& url, ParsedUrl& parsed) {
    int schemeSep = url.indexOf("://");
    if (schemeSep == -1) {
        return false;
    }
    String scheme = url.substring(0, schemeSep);
    parsed.https = scheme.equalsIgnoreCase("https");
    int hostStart = schemeSep + 3;
    int pathStart = url.indexOf('/', hostStart);
    String hostPort = pathStart == -1 ? url.substring(hostStart) : url.substring(hostStart, pathStart);
    if (hostPort.length() == 0) {
        return false;
    }
    int colon = hostPort.indexOf(':');
    if (colon == -1) {
        parsed.host = hostPort;
        parsed.port = parsed.https ? 443 : 80;
    } else {
        parsed.host = hostPort.substring(0, colon);
        parsed.port = static_cast<uint16_t>(hostPort.substring(colon + 1).toInt());
        if (parsed.port == 0) {
            parsed.port = parsed.https ? 443 : 80;
        }
    }
    if (pathStart == -1) {
        parsed.path = "/";
    } else {
        parsed.path = url.substring(pathStart);
        if (parsed.path.length() == 0) {
            parsed.path = "/";
        }
    }
    return true;
}

