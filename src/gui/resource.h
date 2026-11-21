/*
 * AIagent98 - Win32 resource identifiers.
 */
#ifndef A98_GUI_RESOURCE_H
#define A98_GUI_RESOURCE_H

#define IDI_APP            1
#define IDR_MAINMENU       100
#define IDR_ACCEL          101

#define IDD_SETTINGS       200
#define IDD_CONFIRM        201
#define IDD_ABOUT          202
#define IDD_FIRSTRUN       203
#define IDD_INPUTLINE      204
#define IDD_EMOJI          205
#define IDD_TOOLS          206

#define IDC_TRANSCRIPT     1000
#define IDC_INPUT          1001
#define IDC_SEND           1002
#define IDC_STOP           1003
#define IDC_SETTINGS       1004
#define IDC_NEW            1005
#define IDC_STATUS         1006
#define IDC_SESSION_LABEL  1007
#define IDC_SESSION        1008
#define IDC_OPEN_WORKSPACE 1009
#define IDC_EMOJI          1010

#define IDM_NEW            40001
#define IDM_SAVE           40002
#define IDM_LOAD           40003
#define IDM_EXIT           40004
#define IDM_SETTINGS       40005
#define IDM_ABOUT          40006
#define IDM_EXPORT         40007
#define IDM_WORKSPACE      40008
#define IDM_TOOLS_LIST     40009
#define IDM_RESEND         40010
#define IDM_SESSION_DELETE 40011
#define IDM_SESSION_RENAME 40012
#define IDM_OPENLOG        40013
#define IDM_EMOJI          40014

#define IDC_PROVIDER       2000
#define IDC_API_BASE       2001
#define IDC_API_KEY        2002
#define IDC_MODEL          2003
#define IDC_WORKSPACE      2004
#define IDC_MAX_STEPS      2005
#define IDC_MAX_TOKENS     2006
#define IDC_TLS_VERIFY     2007
#define IDC_ALLOW_SHELL    2008
#define IDC_STREAM         2009
#define IDC_SYSTEM_PROMPT  2010
#define IDC_TEMPERATURE    2011
#define IDC_EXTRA_HEADERS  2012
#define IDC_TEST           2013
#define IDC_TEST_RESULT    2014
#define IDC_FULL_ACCESS    2015
#define IDC_AUTO_CONFIRM   2016

/* Settings tabs */
#define IDC_SETTINGS_TAB   2017
#define IDC_ALLOW_OUTSIDE  2018
#define IDC_EMOJI_INLINE   2019
#define IDC_CONNECT_TIMEOUT 2020
#define IDC_REQUEST_TIMEOUT 2021
#define IDC_LOG_LEVEL      2022
#define IDC_NET_ALLOW      2023
#define IDC_NET_ALLOW_HOSTS 2024
#define IDC_NET_MAX_BYTES  2025
#define IDC_NET_SEARCH_URL 2026
#define IDC_NET_GITHUB_TOKEN 2027

/* Settings page labels (so they can be shown/hidden with their page) */
#define IDC_L_PROVIDER  2030
#define IDC_L_API_BASE  2031
#define IDC_L_API_KEY   2032
#define IDC_L_MODEL     2033
#define IDC_L_PROMPT    2034
#define IDC_L_TEMP      2035
#define IDC_L_MAXTOK    2036
#define IDC_L_MAXSTEP   2037
#define IDC_L_HEADERS   2038
#define IDC_L_PRESET    2039
#define IDC_L_WORKSPACE 2040
#define IDC_L_CONNTO    2041
#define IDC_L_REQTO     2042
#define IDC_L_LOG       2043
#define IDC_L_WARN1     2044
#define IDC_L_WARN2     2045
#define IDC_L_HOSTS     2050
#define IDC_L_MAXBYTES  2051
#define IDC_L_SEARCHURL 2052
#define IDC_L_TOKEN     2053
#define IDC_L_NET1      2054
#define IDC_L_NET2      2055
#define IDC_L_NET3      2056
#define IDC_TOOLS_LIST  2060
#define IDC_TOOLS_DETAIL 2061

#define IDC_CONFIRM_TOOL   2100
#define IDC_CONFIRM_ARGS   2101

#define IDC_INPUTLINE_LABEL 2102
#define IDC_INPUTLINE_EDIT  2103

#define IDC_EMOJI_LIST     2104
#define IDC_EMOJI_FILTER   2105
#define IDC_EMOJI_HINT     2106

#endif /* A98_GUI_RESOURCE_H */
