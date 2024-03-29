/*
 * Copyright (c) 2002, 2003, 2004, Scott Nicol <esniper@users.sf.net>
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* for strcasestr  prototype in string.h */
#define _GNU_SOURCE

#include "util.h"
#include "auction.h"
#include "buffer.h"
#include "http.h"
#include "html.h"
#include "history.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <curl/curl.h>
#include <curl/easy.h>
#if defined(WIN32)
#	define strcasecmp(s1, s2) stricmp((s1), (s2))
#	define sleep(t) _sleep((t) * 1000)
#	define strncasecmp(s1, s2, len) strnicmp((s1), (s2), (len))
#else
#	include <unistd.h>
#endif

#define newRemain(aip) (aip->endTime - time(NULL) - aip->latency - options.bidtime)

#define TOKEN_FOUND_UIID (1 << 0)
#define TOKEN_FOUND_STOK (1 << 1)
#define TOKEN_FOUND_SRT (1 << 2)

#define TOKEN_FOUND_ALL (TOKEN_FOUND_UIID | TOKEN_FOUND_STOK | TOKEN_FOUND_SRT)

typedef struct _headerattr
{
  char* name;
  int   occurence;
  int   direction;
  int   mandatory;
  char* value;
} headerAttr_t, headerVal_t, jsonVal_t;

typedef enum searchType { st_attribute, st_value, st_json } searchType_t;

typedef struct _transfercontent
{
  int dest, src, escape;
} transfercontent_t;

static time_t loginTime = 0;	/* Time of last login */
static time_t defaultLoginInterval = 12 * 60 * 60;	/* ebay login interval */

size_t placemakerLen = strlen("%s");

static int acceptBid(const char *pagename, auctionInfo *aip);
static int bid(auctionInfo *aip);
static int ebayLogin(auctionInfo *aip, time_t interval);
static int findAttr(char* src, size_t srcLen, headerAttr_t* attr, char* label);
static int forceEbayLogin(auctionInfo *aip);
static char *getIdInternal(char *s, size_t len);
static int getInfoTiming(auctionInfo *aip, time_t *timeToFirstByte);
static int getQuantity(int want, int available);
static int getJson(char* src, size_t srcLen, jsonVal_t* vals, char* label);
static int getVals(char* src, size_t srcLen, headerVal_t* vals, char* label);
static int makeBidError(const pageInfo_t *pageInfo, auctionInfo *aip);
static int parseBid(memBuf_t *mp, auctionInfo *aip);
static int preBid(auctionInfo *aip);
static int parseHtmlSource(char* src, size_t srcLen, headerAttr_t* searchdef, searchType_t searchfor, char* label);
static int parsePreBid(memBuf_t *mp, auctionInfo *aip);
static int printMyItemsRow(char *row);
static int watch(auctionInfo *aip);

static const char PAGEID[] = "Page id: ";
static const char SRCID[] = "srcId: ";

/*
 * Get page info, including pagename variable, page id and srcid comments.
 */
pageInfo_t *
getPageInfo(memBuf_t *mp)
{
	const char *line;
	pageInfo_t p = {NULL, NULL, NULL}, *pp;
	int needPageName = 1;
	int needPageId = 1;
	int needSrcId = 1;
	int needMore = 3;
	char *title = NULL;

	log(("getPageInfo():\n"));
	memReset(mp);
	while (needMore && (line = getTag(mp))) {
		char *tmp;

		if (!strcasecmp(line, "title") ||
                    !strcasecmp(line, "h1 class=\"page-title__main\"")) {  /* Version newer than 2.33.0 */
		    line = getNonTag(mp);
		    if (line) title = myStrdup(line);
		    continue;
		}
		if (strncmp(line, "!--", 3))
			continue;
		if (needPageName && (tmp = strstr(line, PAGENAME))) {
			if ((tmp = getPageNameInternal(tmp))) {
				--needMore;
				--needPageName;
				p.pageName = myStrdup(tmp);
			}
		} else if (needPageId && (tmp = strstr(line, PAGEID))) {
			if ((tmp = getIdInternal(tmp, sizeof(PAGEID)))) {
				--needMore;
				--needPageId;
				p.pageId = myStrdup(tmp);
			}
		} else if (needSrcId && (tmp = strstr(line, SRCID))) {
			if ((tmp = getIdInternal(tmp, sizeof(SRCID)))) {
				--needMore;
				--needSrcId;
				p.srcId = myStrdup(tmp);
			}
		}
	}
	if (needPageName && title) {
	   log(("using title as page name: %s", title));
	   p.pageName = title;
	   --needPageName;
	   --needMore;
	   title = NULL;
	}
	if (title) free(title);
	log(("getPageInfo(): pageName = %s, pageId = %s, srcId = %s\n", nullStr(p.pageName), nullStr(p.pageId), nullStr(p.srcId)));
	memReset(mp);
	if (needMore == 3)
		return NULL;
	pp = (pageInfo_t *)myMalloc(sizeof(pageInfo_t));
	pp->pageName = p.pageName;
	pp->pageId = p.pageId;
	pp->srcId = p.srcId;
	return pp;
}

static char *
getIdInternal(char *s, size_t len)
{
	char *id = s + len - 1;
	char *dash = strchr(id, '-');

	if (!*dash) {
		log(("getIdInternal(): Cannot find trailing dash: %s\n", id));
		return NULL;
	}
	*dash = '\0';
	log(("getIdInternal(): id = %s\n", id));
	return id;
}

/*
 * Free a pageInfo_t and it's internal members.
 */
void
freePageInfo(pageInfo_t *pp)
{
	if (pp) {
		free(pp->pageName);
		free(pp->pageId);
		free(pp->srcId);
		free(pp);
	}
}

/*
 * Calculate quantity to bid on.  If it is a dutch auction, never
 * bid on more than 1 less item than what is available.
 */
static int
getQuantity(int want, int available)
{
	if (want == 1 || available == 1)
		return 1;
	if (available > want)
		return want;
	return available - 1;
}

static const char HISTORY_URL[] = "https://%s/ws/eBayISAPI.dll?ViewBids&item=%s";

/*
 * getInfo(): Get info on auction from bid history page.
 *
 * returns:
 *	0 OK
 *	1 error (badly formatted page, etc) set auctionError
 */
int
getInfo(auctionInfo *aip)
{
	return getInfoTiming(aip, NULL);
}

/*
 * getInfoTiming(): Get info on auction from bid history page.
 *
 * returns:
 *	0 OK
 *	1 error (badly formatted page, etc) set auctionError
 */
static int
getInfoTiming(auctionInfo *aip, time_t *timeToFirstByte)
{
	int i, ret;
	time_t start;

	log(("\n\n*** getInfo auction %s price %s user %s\n", aip->auction, aip->bidPriceStr, options.username));
	if (ebayLogin(aip, 0))
		return 1;

	for (i = 0; i < 3; ++i) {
		memBuf_t *mp = NULL;

		if (!aip->query) {
			size_t urlLen = sizeof(HISTORY_URL) + strlen(options.historyHost) + strlen(aip->auction) - (2*placemakerLen) + 1;

			aip->query = (char *)myMalloc(urlLen);
			sprintf(aip->query, HISTORY_URL, options.historyHost, aip->auction);
		}
		start = time(NULL);
		if (!(mp = httpGet(aip->query, NULL))) {
			freeMembuf(mp);
			return httpError(aip);
		}
		ret = parseBidHistory(mp, aip, start, timeToFirstByte, 0);
		freeMembuf(mp);
		if (i == 0 && ret == 1 && aip->auctionError == ae_mustsignin) {
			if (forceEbayLogin(aip))
				break;
		} else if (aip->auctionError == ae_notime)
			/* Blank time remaining -- give it another chance */
			sleep(2);
		else
			break;
	}
	return ret;
}

/*
 * Note: quant=1 is just to dupe eBay into allowing the pre-bid to get
 *	 through.  Actual quantity will be sent with bid.
 */
static const char PRE_BID_URL[] = "https://%s/ws/eBayISAPI.dll?MfcISAPICommand=MakeBid&fb=2&co_partner_id=&item=%s";
static const char PRE_BID_DATA[] = "maxbid=%s&quant=%s";

/*
 * Get bid key
 *
 * returns 0 on success, 1 on failure.
 */
static int
preBid(auctionInfo *aip)
{
	memBuf_t *mp = NULL;
	int quantity = getQuantity(options.quantity, aip->quantity);
	char quantityStr[12];	/* must hold an int */
	size_t urlLen, dataLen;
	char *url, *data;
	int ret = 0;

	if (ebayLogin(aip, 0))
		return 1;
	sprintf(quantityStr, "%d", quantity);
	/* create url */ 
	urlLen = sizeof(PRE_BID_URL) + strlen(options.prebidHost) + strlen(aip->auction) - (2*placemakerLen) + 1;
	url = (char *)myMalloc(urlLen);
	sprintf(url, PRE_BID_URL, options.prebidHost, aip->auction);
	/* create data */
	dataLen = sizeof(PRE_BID_DATA) + strlen(aip->bidPriceStr) + strlen(quantityStr) - (2*placemakerLen) + 1;
	data = (char *)myMalloc(dataLen);
	sprintf(data, PRE_BID_DATA, aip->bidPriceStr, quantityStr);
	log(("\n\n*** preBid(): url is %s with data %s\n", url, data));
	mp = httpPost(url, data, NULL);
	free(url);
	free(data);
	if (!mp)
		return httpError(aip);

	ret = parsePreBid(mp, aip);
	freeMembuf(mp);
	return ret;
}

static headerVal_t bidVals[] = {"uiid", 1, -1, 0, NULL,
                                "stok", 1, -1, 0, NULL,
                                "srt", 1, -1, 0, NULL};

static int
parsePreBid(memBuf_t *mp, auctionInfo *aip)
{
	int i;
	char** bidInfo[] = {&aip->biduiid, &aip->bidstok, &aip->bidsrt}; // Points to target items in aip
	int ret = 0;
	int found = 0; //Used as binary store - 1=uiid,10=stok,100=srt

	memReset(mp);

	// Get values needed
	for(i = 0; i < sizeof(bidVals)/sizeof(headerAttr_t); i++)
		if(!getVals(mp->memory, mp->size, &bidVals[i], "parsePreBid") && strlen(bidVals[i].value) > 0) {
			free(*bidInfo[i]);
			*bidInfo[i] = (char*)myMalloc(strlen(bidVals[i].value) + 1);
			strncpy(*bidInfo[i], bidVals[i].value, strlen(bidVals[i].value) + 1);
			found |= (1 << i);
		}
		else {
			ret = -1;
			break;
		}
	// Free memory
	for(i = 0; i < sizeof(bidVals)/sizeof(headerAttr_t); free(bidVals[i++].value));

	if ((found & TOKEN_FOUND_ALL) != TOKEN_FOUND_ALL) {
		pageInfo_t *pageInfo = getPageInfo(mp);

		ret = makeBidError(pageInfo, aip);
		if (ret < 0) {
			ret = auctionError(aip, ae_bidtokens, NULL);
			bugReport("preBid", __FILE__, __LINE__, aip, mp, optiontab, "cannot find bid token (found=%d)", found);
		}
		freePageInfo(pageInfo);
	}

	return ret;
}

static const char LOGIN_1_URL[] = "https://%s/signin/s";
static const char LOGIN_2_URL[] = "https://%s/signin/s";

/* Parameter interpretation
 * ------------------------
 * i1        : ? (= '')
 * pageType  : ? (= '-1')
 * rqid      : <rqid from html source>
 * lkd..guid : rqid or <lkd..guid from html source>
 * mid       : <mid from globalDfpContext>
 * srt       : <srt from html source>%257Cht5new%253Dfalse%2526usid%253D<usid>
 * rtmData   : ? (= 'PS%3DT.0')
 * usid      : <session_id from globalDfpContext>
 * .         : ?
 * .         : ?
 * userid    : Your ebay username
 * pass      : Your ebay password 
 * kmsi      : Keep me signed in (= '1')
 * rdrlog    : %7B%22fp%22%3A%22%22%2C%22prfdcscmd%22%3A%22<rqid>%22%2C%22iframe%22%3Afalse%2C%22timestamp%22%3A<timestamp utc><mmm>%7D (currently empty)
 */

static const char LOGIN_DATA[] = "i1=&\
pageType=-1&\
rqid=%s&\
lkdhjebhsjdhejdshdjchquwekguid=%s&\
mid=%s&\
srt=%s%%257Cht5new%%253Dfalse%%2526usid%%253D%s&\
rtmData=PS%%3DT.0&\
usid=%s&\
htmid=&\
fypReset=&\
ICurl=&\
src=&\
AppName=&\
srcAppId=&\
errmsg=&\
defaultKmsi=true&\
userid=%s&\
pass=%s&\
kmsi=1&\
rdrlog=";

static const char* JAVAISSUE = "Validating JavaScript Engine";

static const char* id1="id=\"";
static const char* id2="value=\"";
static const char* id3=":\"";

static const int USER_NUM=0;
static const int PASS_NUM=1;

static const int RQID=0;
static const int GUID=1;
static const int REGURL=2;
static const int MID=3;
static const int SRT=4;
static const int USID=5;
static const int RUNID2=6;

static headerAttr_t headerAttrs[] = {"<label for=\"userid\"", 1, 1, 0, NULL,
                            "\"password\"", 1, -1, 0, NULL};

static headerVal_t headerVals[] = {"rqid", 1, 1, 1, NULL,
                           "lkdhjebhsjdhejdshdjchquwekguid", 1, 1, 1, NULL,
                           "regUrl", 1, 1, 0, NULL,
                           "mid", 1, 1, 1, NULL,
                           "srt", 1, 1, 1, NULL,
                           "usid", 1, 1, 1, NULL,
                           "runId2", 1, 1, 0, NULL};

static jsonVal_t globalDfpContext[] = {"\"mid\"", 1, 1, 1, NULL,
			   "\"tmxSessionId\"", 1, 1, 1, NULL};

static transfercontent_t transfercontent[] = { 3, 0, 1,
			   5, 1, 0};

static int
parseHtmlSource(char* src, size_t srcLen, headerAttr_t* searchdef, searchType_t searchfor, char* label)
{
	char* start = src;
	char* end = src + srcLen;
	char* search = NULL;
	char pattern[128];
	char res[4096];
	int  i;

	if(searchfor == st_attribute)
		strcpy(pattern, searchdef->name);
	else if(searchfor == st_value)
		sprintf(pattern, "name=\"%s\"", searchdef->name);
	else
		strcpy(pattern, searchdef->name);

	for(i = 0; i < searchdef->occurence; i++) {
		search = strstr(start, pattern);
		if( search == NULL )
		{
			searchdef->value = (char *)myMalloc(1);
			strncpy(searchdef->value, "\0", 1);
			return searchdef->mandatory;
		}
		start = search;
		start += strlen(pattern);
	}

	while(src != search && end != search ) {
                search += (searchdef->direction);

		if(!strncmp(search, (searchfor == st_attribute ? id1 : (searchfor == st_value ? id2 : id3)), 
                                       (searchfor == st_attribute ? strlen(id1) : (searchfor == st_value ? strlen(id2) : strlen(id3)) ) ) ) {
			search += (searchfor == st_attribute ? strlen(id1) : strlen(id2));
			memset(res, '\0', sizeof(res));
			for(i = 0; (*search) != '"' && i < sizeof(res); res[i++] = *search++);
			searchdef->value = (char *)myMalloc(strlen(res) + 1);
			strncpy(searchdef->value, (char*) &res, strlen(res) + 1);
			if (options.debug)
				dlog("%s(): %s=%s", (label != NULL ? label : (searchfor == st_attribute ? "findAttr" : "searchvalue")), 
					searchdef->name, searchdef->value);
			return 0;
		}
	}

	if( searchdef->value == NULL )
        {
            searchdef->value = (char *)myMalloc(1);
            strncpy(searchdef->value, "\0", 1);
            if (options.debug)
               dlog("%s(): %s=%s", (label != NULL ? label : (searchfor == st_attribute ? "findAttr" : "searchvalue")),
                       searchdef->name, searchdef->value);
            return searchdef->mandatory;
        }
	return searchdef->mandatory;
}

static int
findAttr(char* src, size_t srcLen, headerAttr_t* attr, char* label)
{
	return parseHtmlSource(src, srcLen, attr, st_attribute, label);
}

static int
getVals(char* src, size_t srcLen, headerVal_t* vals, char* label)
{
	return parseHtmlSource(src, srcLen, vals, st_value, label);
}

static int
getJson(char* src, size_t srcLen, jsonVal_t* vals, char* label)
{
        return parseHtmlSource(src, srcLen, vals, st_json, label);
}


/*
 * Force an ebay login.
 *
 * Returns 0 on success, 1 on failure.
 */
static int
forceEbayLogin(auctionInfo *aip)
{
	loginTime = 0;
	return ebayLogin(aip, 0);
}

/*
 * Ebay login.  Make sure loging has been done with the given interval.
 *
 * Returns 0 on success, 1 on failure.
 */
static int
ebayLogin(auctionInfo *aip, time_t interval)
{
	memBuf_t *mp = NULL;
	size_t urlLen;
	char *url, *data, *logdata;
	pageInfo_t *pp;
	int ret = 0;
	char *password;
	int i;	
	int noBugReport=0;

	/* negative value forces login */
	if (loginTime > 0) {
		if (interval == 0)
			interval = defaultLoginInterval;	/* default: 12 hours */
		if ((time(NULL) - loginTime) <= interval)
			return 0;
	}

	cleanupCurlStuff();
	if (initCurlStuff())
		return auctionError(aip, ae_unknown, NULL);

        urlLen = sizeof(LOGIN_2_URL) + strlen(options.loginHost) - (1*placemakerLen) + 1;
        url = (char *)myMalloc(urlLen);
        sprintf(url, LOGIN_2_URL, options.loginHost);
        mp = httpPost(url, NULL, NULL); /* Send fake login in order to get wanted html data */
        free(url);

	if (!mp)
		return httpError(aip);

	// In some cases ebay is checking for java script
        if ((pp = getPageInfo(mp)) && !strncasecmp(pp->pageName, JAVAISSUE, strlen(JAVAISSUE))) {
		noBugReport=1;
		printLog(stdout, "# Warning: Pagename \"%s\" found (Response to fake login).\n", pp->pageName);
	}

	// Get all attributes and values needed
	for(i = 0; i < sizeof(headerAttrs)/sizeof(headerAttr_t); i++)
		if(findAttr(mp->memory, mp->size, &headerAttrs[i], NULL))
			if(!noBugReport) bugReport("ebayLogin", __FILE__, __LINE__, aip, mp, optiontab,
				"findAttr cannot find %s (headerAttrs)", headerAttrs[i].name);
	for(i = 0; i < sizeof(headerVals)/sizeof(headerVal_t); i++)
		if(getVals(mp->memory, mp->size, &headerVals[i], NULL))
			if(!noBugReport) bugReport("ebayLogin", __FILE__, __LINE__, aip, mp, optiontab,
				"getVals cannot find %s (headerVals)", headerVals[i].name);
	for(i = 0; i < sizeof(globalDfpContext)/sizeof(jsonVal_t); i++)
                if(getJson(mp->memory, mp->size, &globalDfpContext[i], NULL))
                        if(!noBugReport) bugReport("ebayLogin", __FILE__, __LINE__, aip, mp, optiontab,
                                "getJson cannot find %s (globalDfpContext)", globalDfpContext[i].name);

	// Transfer json
	for(i = 0; i < sizeof(transfercontent)/sizeof(transfercontent_t); i++)
		if( strlen(headerVals[transfercontent[i].dest].value) == 0 ) {
			free(headerVals[transfercontent[i].dest].value);
			if( transfercontent[i].escape > 0 ) {
				data = curl_escape(globalDfpContext[transfercontent[i].src].value, 0);
				headerVals[transfercontent[i].dest].value = (char*)myMalloc(strlen(data) + 1);
				strcpy(headerVals[transfercontent[i].dest].value, data);
				curl_free(data);
			}
			else {
				headerVals[transfercontent[i].dest].value = (char*)myMalloc(strlen(globalDfpContext[transfercontent[i].src].value) + 1);
				strcpy(headerVals[transfercontent[i].dest].value, globalDfpContext[transfercontent[i].src].value);
			}
                        if (options.debug)
                                dlog("%s copied", headerVals[transfercontent[i].dest].value); 
		}

	freeMembuf(mp);
	mp = NULL;

	/* create url */
	urlLen = sizeof(LOGIN_2_URL) + strlen(options.loginHost) - (1*placemakerLen) + 1;
	password = getPassword();
	url = (char *)myMalloc(urlLen);
	sprintf(url, LOGIN_2_URL, options.loginHost);
	/* create data + log */
	data = (char *)myMalloc(	sizeof(LOGIN_DATA)
                                      + strlen(options.usernameEscape)
                                      + strlen(password)
                                      + strlen(headerVals[RQID].value)
                                      + strlen(headerVals[GUID].value)
                                      + strlen(headerVals[MID].value)
                                      + strlen(headerVals[SRT].value)
                                      + strlen(headerVals[USID].value)
                                      + strlen(headerVals[USID].value)
				      - (8*placemakerLen) + 1
                                      );
	logdata = (char *)myMalloc(	sizeof(LOGIN_DATA)
                                      + strlen(options.usernameEscape)
                                      + 5
                                      + strlen(headerVals[RQID].value)
                                      + strlen(headerVals[GUID].value)
                                      + strlen(headerVals[MID].value)
                                      + strlen(headerVals[SRT].value)
                                      + strlen(headerVals[USID].value)
                                      + strlen(headerVals[USID].value)
				      - (8*placemakerLen) + 1
                                      );
	sprintf(data, LOGIN_DATA,	headerVals[RQID].value,
					headerVals[GUID].value,
					headerVals[MID].value,
					headerVals[SRT].value,
					headerVals[USID].value,
					headerVals[USID].value,
					options.usernameEscape,
					password
					);
	freePassword(password);
	sprintf(logdata, LOGIN_DATA,	headerVals[RQID].value,
					headerVals[GUID].value,
					headerVals[MID].value,
					headerVals[SRT].value,
					headerVals[USID].value,
					headerVals[USID].value,
					options.usernameEscape,
					"*****"
					);

	// Using POST method instead of GET
	mp = httpPost(url, data, logdata);

	// Free memory
	for(i=0; i < sizeof(headerAttrs)/sizeof(headerAttr_t); free(headerAttrs[i++].value));
	for(i=0; i < sizeof(headerVals)/sizeof(headerVal_t); free(headerVals[i++].value));
	for(i=0; i < sizeof(globalDfpContext)/sizeof(jsonVal_t); free(globalDfpContext[i++].value));
	free(url);
	free(data);
	free(logdata);

	if (!mp)
		return httpError(aip);

	if ((pp = getPageInfo(mp))) {
		log(("ebayLogin(): pagename = \"%s\", pageid = \"%s\", srcid = \"%s\"", nullStr(pp->pageName), nullStr(pp->pageId), nullStr(pp->srcId)));
		/*
		 * Pagename is usually MyeBaySummary, but it seems as though
		 * it can be any MyeBay page, and eBay is not consistent with
		 * naming of MyeBay pages (MyeBay, MyEbay, myebay, ...) so
		 * esniper must use strncasecmp().
		 */
		if ((pp->srcId && !strcmp(pp->srcId, "SignInAlertSupressor"))||
		    (pp->pageName &&
			(!strncasecmp(pp->pageName, "MyeBay", 6) ||
			 !strncasecmp(pp->pageName, "My eBay", 7) ||
			 !strncasecmp(pp->pageName, "Watch list", 10) ||
			 !strncasecmp(pp->pageName, "Purchase History", 16) ||
			 !strncasecmp(pp->pageName, " Black Friday", 13) ||
			 !strncasecmp(pp->pageName, "Black Friday", 12) ||
			 !strncasecmp(pp->pageName, "Electronics", 11) ||
                         !strncasecmp(pp->pageName, "eBay: Update your contact info", 30))
		    ))
			loginTime = time(NULL);
		else if (pp->pageName &&
			 !strncasecmp(pp->pageName, JAVAISSUE, strlen(JAVAISSUE))) {
			loginTime = time(NULL);
			printLog(stdout, "# Warning: Pagename \"%s\" found (Response to login).\n", pp->pageName);
		}
		else if (pp->pageName &&
				(!strcmp(pp->pageName, "Welcome to eBay") ||
				 !strcmp(pp->pageName, "Welcome to eBay - Sign in - Error") ||
				 !strcmp(pp->pageName, "Welcome to eBay - Error") ||
				 !strcmp(pp->pageName, "Sign in or Register | eBay - Error")))
			ret = auctionError(aip, ae_badpass, NULL);
		else if (pp->pageName && !strcmp(pp->pageName, "PageSignIn"))
			ret = auctionError(aip, ae_login, NULL);
		else if (pp->pageName && !strcmp(pp->pageName, "Reset your password"))
			ret = auctionError(aip, ae_manualaction, NULL);
		else if (pp->srcId && !strcmp(pp->srcId, "Captcha.xsl"))
			ret = auctionError(aip, ae_captcha, NULL);
		else {
			ret = auctionError(aip, ae_login, NULL);
			bugReport("ebayLogin", __FILE__, __LINE__, aip, mp, optiontab, "unknown pageinfo");
		}
	} else {
		log(("ebayLogin(): pageinfo is NULL\n"));
		ret = auctionError(aip, ae_login, NULL);
		bugReport("ebayLogin", __FILE__, __LINE__, aip, mp, optiontab, "pageinfo is NULL");
	}
	freeMembuf(mp);
	freePageInfo(pp);
	return ret;
}

/*
 * acceptBid: handle all known AcceptBid pages.
 *
 * Returns -1 if page not recognized, 0 if bid accepted, 1 if bid not accepted.
 */
static int
acceptBid(const char *pagename, auctionInfo *aip)
{
	static const char ACCEPTBID[] = "AcceptBid_";
	static const char HIGHBID[] = "HighBidder";
	static const char OUTBID[] = "Outbid";
	static const char RESERVENOTMET[] = "ReserveNotMet";

	if (!strcmp(pagename, "Bid confirmation"))
		return aip->bidResult = 0;

	if (!pagename ||
	    strncmp(pagename, ACCEPTBID, sizeof(ACCEPTBID) - 1))
		return -1;
	pagename += sizeof(ACCEPTBID) - 1;

	/*
	 * valid pagenames include AcceptBid_HighBidder,
	 * AcceptBid_HighBidder_rebid, possibly others.
	 */
	if (!strncmp(pagename, HIGHBID, sizeof(HIGHBID) - 1))
		return aip->bidResult = 0;
	/*
	 * valid pagenames include AcceptBid_Outbid, AcceptBid_Outbid_rebid,
	 * possibly others.
	 */
	if (!strncmp(pagename, OUTBID, sizeof(OUTBID) - 1))
		return aip->bidResult = auctionError(aip, ae_outbid, NULL);
	/*
	 * valid pagenames include AcceptBid_ReserveNotMet,
	 * AcceptBid_ReserveNotMet_rebid, possibly others.
	 */
	if (!strncmp(pagename, RESERVENOTMET, sizeof(RESERVENOTMET) - 1))
		return aip->bidResult = auctionError(aip, ae_reservenotmet, NULL);
	/* unknown AcceptBid page */
	return -1;
}

/*
 * makeBidError: handle all known MakeBidError pages.
 *
 * Returns -1 if page not recognized, 0 if bid accepted, 1 if bid not accepted.
 */
static int
makeBidError(const pageInfo_t *pageInfo, auctionInfo *aip)
{
	static const char MAKEBIDERROR[] = "MakeBidError";
	const char *pagename = pageInfo->pageName;

	if (!pagename) {
		const char *srcId = pageInfo->srcId;

		if (srcId && !strcasecmp(srcId, "ViewItem"))
			return aip->bidResult = auctionError(aip, ae_ended, NULL);
		else
			return -1;
	}
	if (!strcasecmp(pagename, "Place bid"))
		return aip->bidResult = auctionError(aip, ae_outbid, NULL);
	if (!strcasecmp(pagename, "eBay Alerts"))
		return aip->bidResult = auctionError(aip, ae_alert, NULL);
	if (!strcasecmp(pagename, "Buyer Requirements"))
		return aip->bidResult = auctionError(aip, ae_buyerrequirements, NULL);

	if (!strcasecmp(pagename, "PageSignIn"))
		return aip->bidResult = auctionError(aip, ae_mustsignin, NULL);
	if (!strncasecmp(pagename, "BidManager", 10) ||
	    !strncasecmp(pagename, "BidAssistant", 12))
		return aip->bidResult = auctionError(aip, ae_bidassistant, NULL);

	if (strncasecmp(pagename, MAKEBIDERROR, sizeof(MAKEBIDERROR) - 1))
		return -1;
	pagename += sizeof(MAKEBIDERROR) - 1;
	if (!*pagename ||
	    !strcasecmp(pagename, "AuctionEnded"))
		return aip->bidResult = auctionError(aip, ae_ended, NULL);
	if (!strcasecmp(pagename, "AuctionEnded_BINblock") ||
	    !strcasecmp(pagename, "AuctionEnded_BINblock "))
		return aip->bidResult = auctionError(aip, ae_cancelled, NULL);
	if (!strcasecmp(pagename, "Password"))
		return aip->bidResult = auctionError(aip, ae_badpass, NULL);
	if (!strcasecmp(pagename, "MinBid"))
		return aip->bidResult = auctionError(aip, ae_bidprice, NULL);
	if (!strcasecmp(pagename, "BuyerBlockPref"))
		return aip->bidResult = auctionError(aip, ae_buyerblockpref, NULL);
	if (!strcasecmp(pagename, "BuyerBlockPrefDoesNotShipToLocation"))
		return aip->bidResult = auctionError(aip, ae_buyerblockprefdoesnotshiptolocation, NULL);
	if (!strcasecmp(pagename, "BuyerBlockPrefNoLinkedPaypalAccount"))
		return aip->bidResult = auctionError(aip, ae_buyerblockprefnolinkedpaypalaccount, NULL);
	if (!strcasecmp(pagename, "HighBidder"))
		return aip->bidResult = auctionError(aip, ae_highbidder, NULL);
	if (!strcasecmp(pagename, "CannotBidOnItem"))
		return aip->bidResult = auctionError(aip, ae_cannotbid, NULL);
	if (!strcasecmp(pagename, "DutchSameBidQuantity"))
		return aip->bidResult = auctionError(aip, ae_dutchsamebidquantity, NULL);
	if (!strcasecmp(pagename, "BuyerBlockPrefItemCountLimitExceeded"))
		return aip->bidResult = auctionError(aip, ae_buyerblockprefitemcountlimitexceeded, NULL);
	if (!strcasecmp(pagename, "BidGreaterThanBin_BINblock"))
		return aip->bidResult = auctionError(aip, ae_bidgreaterthanbin_binblock, NULL);
	/* unknown MakeBidError page */
	return -1;
}

/*
 * Parse bid result.
 *
 * Returns:
 * 0: OK
 * 1: error
 */
static int
parseBid(memBuf_t *mp, auctionInfo *aip)
{
	/*
	 * The following sometimes have more characters after them, for
	 * example AcceptBid_HighBidder_rebid (you were already the high
	 * bidder and placed another bid).
	 */
	pageInfo_t *pageInfo = getPageInfo(mp);
	int ret;

	aip->bidResult = -1;
	log(("parseBid(): pagename = %s\n", pageInfo->pageName));
	if ((ret = acceptBid(pageInfo->pageName, aip)) >= 0 ||
	    (ret = makeBidError(pageInfo, aip)) >= 0) {
		;
	} else {
		bugReport("parseBid", __FILE__, __LINE__, aip, mp, optiontab, "unknown pagename");
		printLog(stdout, "Cannot determine result of bid\n");
		ret = 0;	/* prevent another bid */
	}
	freePageInfo(pageInfo);
	return ret;
} /* parseBid() */

static const char BID_URL[] = "https://%s/ws/eBayISAPI.dll?MfcISAPICommand=MakeBid&mode=1&co_partnerid=2fb=2&item=%s";
static const char BID_DATA[] = "user=%s&maxbid=%s&quant=%s&uiid=%s&stok=%s&srt=%s";
static const char BID_LOG[] = "bid(): url=%s with data %s";

/*
 * Place bid.
 *
 * Returns:
 * 0: OK
 * 1: error
 */
static int
bid(auctionInfo *aip)
{
	memBuf_t *mp = NULL;
	size_t urlLen, dataLen, logLen, logLen2;
	char *url, *data, *logdata, *logdata2, *tmpStars;
	int ret;
	int quantity = getQuantity(options.quantity, aip->quantity);
	char quantityStr[12];	/* must hold an int */

	if (!aip->biduiid || !aip->bidstok || !aip->bidsrt)
		return auctionError(aip, ae_bidtokens, NULL);

	if (ebayLogin(aip, 0))
		return 1;
	sprintf(quantityStr, "%d", quantity);

	/* create url */
	urlLen = sizeof(BID_URL) + strlen(options.bidHost) + strlen(aip->auction) - (2*placemakerLen) + 1;
	url = (char *)myMalloc(urlLen);
	sprintf(url, BID_URL, options.bidHost, aip->auction);
	/* create data */
	dataLen = sizeof(BID_DATA) + strlen(options.usernameEscape) + strlen(aip->bidPriceStr) + strlen(quantityStr) + strlen(aip->biduiid) + strlen(aip->bidstok) + strlen(aip->bidsrt) - (6*placemakerLen) + 1;
        data = (char *)myMalloc(dataLen);
        sprintf(data, BID_DATA, options.usernameEscape, aip->bidPriceStr, quantityStr, aip->biduiid, aip->bidstok, aip->bidsrt);
	/* create log */
	tmpStars = stars(3);
	logLen2 = sizeof(BID_DATA) + (6*strlen(tmpStars)) - (6*placemakerLen) + 1; 
	logdata2 = (char *)myMalloc(logLen2);
	sprintf(logdata2, BID_DATA, tmpStars, tmpStars, tmpStars, tmpStars, tmpStars, tmpStars);
	logLen = strlen(BID_LOG) - (2*placemakerLen) + urlLen + logLen2 + 1;
	logdata = (char *)myMalloc(logLen);
	sprintf(logdata, BID_LOG, url, logdata2); 

	if (!options.bid) {
		printLog(stdout, "Bidding disabled\n");
		log(("\n\nbid(): query url:\n%s\n\twith data: %s\n", url, data));
		ret = aip->bidResult = 0;
	} else if (!(mp = httpPost(url, data, logdata))) {
		ret = httpError(aip);
	} else {
		ret = parseBid(mp, aip);
	}
	free(url);
	free(data);
	free(logdata);
	free(logdata2);
	free(tmpStars);
	freeMembuf(mp);
	return ret;
} /* bid() */

/*
 * watch(): watch auction until it is time to bid
 *
 * returns:
 *	0 OK
 *	1 Error
 */
static int
watch(auctionInfo *aip)
{
	int errorCount = 0;
	long remain = LONG_MIN;
	unsigned int sleepTime = 0;

	log(("*** WATCHING auction %s price-each %s quantity %d bidtime %ld\n", aip->auction, aip->bidPriceStr, options.quantity, options.bidtime));

	for (;;) {
		time_t tmpLatency;
		time_t start = time(NULL);
		time_t timeToFirstByte = 0;
		int ret = getInfoTiming(aip, &timeToFirstByte);
		time_t end = time(NULL);

		if (timeToFirstByte == 0)
			timeToFirstByte = end;
		tmpLatency = (timeToFirstByte - start);
		if ((tmpLatency >= 0) && (tmpLatency < 600))
			aip->latency = tmpLatency;
		printLog(stdout, "Latency: %d seconds\n", aip->latency);

		if (ret) {
			printAuctionError(aip, stderr);

			/*
			 * Fatal error?  We allow up to 50 errors, then quit.
			 * eBay "unavailable" doesn't count towards the total.
			 */
			if (aip->auctionError == ae_unavailable) {
				if (remain >= 0)
					remain = newRemain(aip);
				if (remain == LONG_MIN || remain > 86400) {
					/* typical eBay maintenance period
					 * is two hours.  Sleep for half that
					 * amount of time.
					 */
					printLog(stdout, "%s: Will try again, sleeping for an hour\n", timestamp());
					sleepTime = 3600;
					sleep(sleepTime);
					continue;
				}
			} else if (remain == LONG_MIN) {
				/* first time through?  Give it 3 chances then
				 * make the error fatal.
				 */
				int j;

				for (j = 0; ret && j < 3 && aip->auctionError == ae_notitle; ++j)
					ret = getInfo(aip);
				if (ret)
					return 1;
				remain = newRemain(aip);
			} else {
				/* non-fatal error */
				log(("ERROR %d!!!\n", ++errorCount));
				if (errorCount > 50)
					return auctionError(aip, ae_toomany, NULL);
				printLog(stdout, "Cannot find auction - internet or eBay problem?\nWill try again after sleep.\n");
				remain = newRemain(aip);
			}
		} else if (!isValidBidPrice(aip))
			return auctionError(aip, ae_bidprice, NULL);
		else
			remain = newRemain(aip);

		/*
		 * Check login when we are close to bidding.
		 */
		if (remain <= 300) {
			if (ebayLogin(aip, defaultLoginInterval - 600))
				return 1;
			remain = newRemain(aip);
		}

		/*
		 * if we're less than two minutes away, get bid key
		 */
		if (remain <= 150 && !aip->biduiid && aip->auctionError == ae_none) {
			int i;

			printf("\n");
			for (i = 0; i < 5; ++i) {
				/* ae_bidtokens is used when the page loaded
				 * but failed for some unknown reason.
				 * Do not try again in this situation.
				 */
				if (!preBid(aip) ||
				    aip->auctionError == ae_bidtokens)
					break;
				if (aip->auctionError == ae_mustsignin &&
				    forceEbayLogin(aip))
					break;
			}
			if (aip->auctionError != ae_none &&
			    aip->auctionError != ae_highbidder) {
				printLog(stderr, "Cannot get bid key\n");
				return 1;
			}
		}

		remain = newRemain(aip);

		/* it's time!!! */
		if (remain <= 0)
			break;

		/*
		 * Setup sleep schedule so we get updates once a day, then
		 * at 2 hours, 1 hour, 5 minutes, 2 minutes
		 */
		if (remain <= 150)	/* 2 minutes + 30 seconds (slop) */
			sleepTime = (unsigned int)remain;
		else if (remain < 720)	/* 5 minutes + 2 minutes (slop) */
			sleepTime = (unsigned int)remain - 120;
		else if (remain < 3900)	/* 1 hour + 5 minutes (slop) */
			sleepTime = (unsigned int)remain - 600;
		else if (remain < 10800)/* 2 hours + 1 hour (slop) */
			sleepTime = (unsigned int)remain - 3600;
		else if (remain < 97200)/* 1 day + 3 hours (slop) */
			sleepTime = (unsigned int)remain - 7200;
		else			/* knock off one day */
			sleepTime = 86400;

		printf("%s: ", timestamp());
		if (sleepTime >= 86400)
			printLog(stdout, "Sleeping for a day\n");
		else if (sleepTime >= 3600)
			printLog(stdout, "Sleeping for %d hours %d minutes\n",
				sleepTime/3600, (sleepTime % 3600) / 60);
		else if (sleepTime >= 60)
			printLog(stdout, "Sleeping for %d minutes %d seconds\n",
				sleepTime/60, sleepTime % 60);
		else
			printLog(stdout, "Sleeping for %ld seconds\n", sleepTime);
		sleep(sleepTime);
		printf("\n");

		if ((remain=newRemain(aip)) <= 0)
			break;
	}
	return 0;
} /* watch() */

/*
 * parameters:
 * aip	auction to bid on
 *
 * return number of items won
 */
int
snipeAuction(auctionInfo *aip)
{
	int won = 0;
	char *tmpUsername;

	if (!aip)
		return 0;

	if (options.debug)
		logOpen(aip, options.logdir);

	tmpUsername = stars(strlen(options.username));
	log(("auction %s price %s quantity %d user %s bidtime %ld\n",
	     aip->auction, aip->bidPriceStr,
	     options.quantity, tmpUsername, options.bidtime));
	free(tmpUsername);

	if (ebayLogin(aip, 0)) {
		printAuctionError(aip, stderr);
		return 0;
	}

	/* 0 means "now" */
	if ((options.bidtime == 0) ? preBid(aip) : watch(aip)) {
		printAuctionError(aip, stderr);
		if (aip->auctionError != ae_highbidder)
			return 0;
	}

	/* ran out of time! */
	if (aip->endTime <= time(NULL)) {
		(void)auctionError(aip, ae_ended, NULL);
		printAuctionError(aip, stderr);
		return 0;
	}

	if (aip->auctionError != ae_highbidder) {
		printLog(stdout, "\nAuction %s: Bidding...\n", aip->auction);
		for (;;) {
			if (bid(aip)) {
				/* failed bid */
				if (aip->auctionError == ae_mustsignin) {
					if (!forceEbayLogin(aip))
						continue;
				}
				printAuctionError(aip, stderr);
				return 0;
			}
			break;
		}
	}

	/* view auction after bid.
	 * Stick it in a loop in case our timing is a bit off (due
	 * to wild swings in latency, for instance).
	 */
	for (;;) {
		if (options.bidtime > 0 && options.bidtime < 60) {
			time_t seconds = aip->endTime - time(NULL);

			if (seconds < 0)
				seconds = 0;
			/* extra 2 seconds to make sure auction is over */
			seconds += 2;
			printLog(stdout, "Auction %s: Waiting %d seconds for auction to complete...\n", aip->auction, seconds);
			sleep((unsigned int)seconds);
		}

		printLog(stdout, "\nAuction %s: Post-bid info:\n",
			 aip->auction);
		if (getInfo(aip))
			printAuctionError(aip, stderr);
		if (aip->remain > 0 && aip->remain < 60 &&
		    options.bidtime > 0 && options.bidtime < 60)
			continue;
		break;
	}

	if (aip->won == -1) {
		won = options.quantity < aip->quantity ?
			options.quantity : aip->quantity;
		printLog(stdout, "\nunknown outcome, assume that you have won %d items\n", won);
	} else {
		won = aip->won;
		printLog(stdout, "\nwon %d item(s)\n", won);
	}
	options.quantity -= won;
	return won;
}

/*
 * Print out items
 */
static int
printMyItemsRow(char *row)
{
	typedef enum _watchsearch { ws_none, ws_attval, ws_text, ws_num } watchsearch_t;
	typedef enum _watchsubsearch { wss_none, wss_decodehtmltwice, wss_converted } watchsubsearch_t;

	typedef struct _watchsrc {
		char* search;
		watchsearch_t method;
		watchsubsearch_t submethod;
	} watchsrc_t;

	const char* ended="ENDED";
	const char* clipped="class=\"clipped\">";
	const char* converted="Converted from ";
	int i, ii=0, num, ret=0;
	char *tmp, *tmp2, buf[64];
	char* ptr[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	char* ptr2[] = {0, 0};
	watchsrc_t watchsrc[] = { "data-itemid=\"", ws_attval, wss_none,
				"aria-label=\"", ws_attval, wss_decodehtmltwice,
				"class=\"item-variations\">", ws_text, wss_none,
				"class=\"price-info\">", ws_num, wss_none,
				"class=\"info-price\">", ws_text, wss_converted,
				"class=\"info-shipping\">", ws_text, wss_converted,
				"class=\"info-timer\">", ws_text, wss_none,
				"class=\"info-username\">", ws_text, wss_none,
				"class=\"info-score\">", ws_text, wss_none,
				"class=\"info-time\">", ws_text, wss_none,
				0, 0, 0};

	/* Ended ? */
	if(strstr(row, ended)) return 0;

	/* Get auction infos */
	for(i=0; i < (sizeof(watchsrc)/sizeof(watchsrc_t)); i++) {
		if(!watchsrc[i].search) continue;

		tmp = strstr(row, watchsrc[i].search);
		if(tmp) {
			tmp+=strlen(watchsrc[i].search);
			switch(watchsrc[i].method) {
				case ws_attval:
					ptr[i] = getNonTagFromString(tmp);
					if((tmp2=strstr(ptr[i], "\""))) *tmp2 = '\0';
					break;
				case ws_text:
					ptr[i] = getNonTagFromString(tmp);
					if((tmp2=strstr(ptr[i], "<"))) *tmp2 = '\0';
					break;
				case ws_num:
					num = getIntFromString(tmp);
					sprintf(buf, "%d", num);
					ptr[i] = myStrdup(buf);
					break;
			}

			switch(watchsrc[i].submethod) {
				case wss_decodehtmltwice:
					tmp2=getNonTagFromString(ptr[i]);
					if(tmp2) {
						free(ptr[i]);
						ptr[i] = myStrdup(tmp2);
					}
					break;	
				case wss_converted:
					tmp2=strstr(tmp, clipped);
					if(tmp2)
					{
						tmp2+=strlen(clipped);
						ptr2[ii] = getNonTagFromString(tmp2);
						if((tmp2=strstr(ptr2[ii], "<"))) *tmp2 = '\0';
						if((tmp2=strstr(ptr2[ii], converted))) {
							sprintf(buf, "(%s)", tmp2+strlen(converted));
							free(ptr2[ii]);
							ptr2[ii] = myStrdup(buf);
						}
						else *ptr2[ii] = '\0';
						ii++;
					}
					break;
			}
		}
	}

	/* Print item */
	printf("Description:\t%s (%s)\n\tItem-Id:\t%s\n\tSeller:\t\t%s %s\n\tBids:\t\t%s\n\tPrice:\t\t%s %s\n\tShipping:\t%s %s\n\tTime left:\t%s\n\n",
		ptr[1], (ptr[2] ? ptr[2] : "---"), ptr[0], ptr[7], ptr[8], ptr[3], ptr[4], ptr2[0], ptr[5], ptr2[1], (ptr[6] ? ptr[6] : ptr[9]));

	/* Free memory */
	for(i=0; i<(sizeof(ptr)/sizeof(char*)); free(ptr[i++]));
	for(i=0; i<(sizeof(ptr2)/sizeof(char*)); free(ptr2[i++]));

	return ret;
}

static const char MYITEMS_URL[] = "https://%s/ws/eBayISAPI.dll?MyeBay&CurrentPage=MyeBayWatching";

/*
 * TODO: allow user configuration of myItems.
 */
int
printMyItems(void)
{
	const char* myitem = "<div class=\"m-item\">";
	memBuf_t *mp = NULL;
	auctionInfo *dummy = newAuctionInfo("0", "0");
	char *url, *next;
	size_t urlLen;

	if (ebayLogin(dummy, 0)) {
		printAuctionError(dummy, stderr);
		freeAuction(dummy);
		return 1;
	}
	urlLen = sizeof(MYITEMS_URL) + strlen(options.myeBayHost) - (1*placemakerLen) + 1;
	url = (char *)myMalloc(urlLen);
	sprintf(url, MYITEMS_URL, options.myeBayHost);
	mp = httpGet(url, NULL);
	free(url);
	if (!mp) {
		httpError(dummy);
		printAuctionError(dummy, stderr);
		freeAuction(dummy);
		freeMembuf(mp);
		return 1;
	}

	/* Loop over watched items */
	mp->memory = strstr(mp->memory, myitem);
	while(mp->memory) {
		next = strstr(mp->memory + strlen(myitem), myitem);
		/* End of item */
		if(next) *(next-1)='\0';
		printMyItemsRow(mp->memory);
		mp->memory = next;
	}

	freeAuction(dummy);
	freeMembuf(mp);
	return 0;
}

/* secret option - test parser */
void
testParser(int flag)
{
	memBuf_t *mp = readFile(stdin);

	switch (flag) {
	case 1:
	    {
		/* print pagename */
		char *line;

		/* dump non-tag data */
		while ((line = getNonTag(mp)))
			printf("\"%s\"\n", line);

		/* pagename? */
		memReset(mp);
		if ((line = getPageName(mp)))
			printf("\nPAGENAME is \"%s\"\n", line);
		else
			printf("\nPAGENAME is NULL\n");
		break;
	    }
	case 2:
	    {
		/* run through bid history parser */
		auctionInfo *aip = newAuctionInfo("1", "2");
		time_t start = time(NULL), end;
		int ret = parseBidHistory(mp, aip, start, &end, 1);

		printf("ret = %d\n", ret);
		printAuctionError(aip, stdout);
		break;
	    }
	case 3:
	    {
		/* run through bid result parser */
		auctionInfo *aip = newAuctionInfo("1", "2");
		int ret = parseBid(mp, aip);

		printf("ret = %d\n", ret);
		printAuctionError(aip, stdout);
		break;
	    }
	case 4:
	    {
		/* print bid history table */
		const char *table;
		char **row;
		char *cp;
		int rowNum = 0;

		while ((cp = getNonTag(mp))) {
			if (!strcmp(cp, "Time left:"))
				break;
			if (!strcmp(cp, "Time Ended:"))
				break;
		}
		if (!cp) {
			printf("time left not found!\n");
			break;
		}
		(void)getTableStart(mp); /* skip one table */
		table = getTableStart(mp);
		if (!table) {
			printf("no table found!\n");
			break;
		}

		printf("table: %s\n", table);
		while ((row = getTableRow(mp))) {
			int columnNum = 0;

			printf("\trow %d:\n", rowNum++);
			for (; row[columnNum]; ++columnNum) {
				memBuf_t buf;

				strToMemBuf(row[columnNum], &buf);
				printf("\t\tcolumn %d: %s\n", columnNum, getNonTag(mp));
				free(row[columnNum]);
			}
		}
		break;
	    }
	case 5:
		{
		/* run through prebid parser */
		auctionInfo *aip = newAuctionInfo("1", "2");
		int ret = parsePreBid(mp, aip);

		printf("ret = %d\n", ret);
		printf("uiid = %s\n", aip->biduiid);
		printAuctionError(aip, stdout);
		break;
		}
	case  6:
		{
		/* Test html parser */
		printf("Output: %s\n", getNonTag(mp));
		break;
		}
	}
}
