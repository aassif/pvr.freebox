#pragma once
/*
 *      Copyright (C) 2018 Aassif Benassarou
 *      http://github.com/aassif/pvr.freebox/
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include <set>
#include <map>
#include <queue>
#include <algorithm> // find_if
#include "kodi/libXBMC_pvr.h"
#include "kodi/libKODI_guilib.h"
#include "p8-platform/os.h"
#include "p8-platform/threads/threads.h"
#include "rapidjson/document.h"

#define PVR_FREEBOX_VERSION "2.0.2"

#define PVR_FREEBOX_BACKEND_NAME "Freebox TV"
#define PVR_FREEBOX_BACKEND_VERSION PVR_FREEBOX_VERSION
#define PVR_FREEBOX_CONNECTION_STRING "1337"

#define PVR_FREEBOX_APP_ID "org.xbmc.freebox"
#define PVR_FREEBOX_APP_NAME "Kodi"
#define PVR_FREEBOX_APP_VERSION PVR_FREEBOX_VERSION

#define PVR_FREEBOX_MENUHOOK_CHANNEL_SOURCE  1
#define PVR_FREEBOX_MENUHOOK_CHANNEL_QUALITY 2

#define PVR_FREEBOX_STRING_CHANNELS_LOADED      30000
#define PVR_FREEBOX_STRING_AUTH_REQUIRED        30001
#define PVR_FREEBOX_STRING_CHANNEL_SOURCE       30008
#define PVR_FREEBOX_STRING_CHANNEL_SOURCE_AUTO  30010
#define PVR_FREEBOX_STRING_CHANNEL_SOURCE_IPTV  30011
#define PVR_FREEBOX_STRING_CHANNEL_SOURCE_DVB   30012
#define PVR_FREEBOX_STRING_CHANNEL_QUALITY      30013
#define PVR_FREEBOX_STRING_CHANNEL_QUALITY_AUTO 30015
#define PVR_FREEBOX_STRING_CHANNEL_QUALITY_HD   30016
#define PVR_FREEBOX_STRING_CHANNEL_QUALITY_SD   30017
#define PVR_FREEBOX_STRING_CHANNEL_QUALITY_LD   30018
#define PVR_FREEBOX_STRING_CHANNEL_QUALITY_3D   30019

template <class K>
class Index
{
  private:
    int              m_id;
    std::map<K, int> m_map;

  public:
    inline
    Index (int first = 0) :
      m_id (first),
      m_map ()
    {
    }

    inline
    int operator() (const K & key)
    {
#if __cplusplus >= 201703L
      auto [i, success] = m_map.emplace (key, m_id);
      return success ? m_id++ : i->second;
#else
      auto r = m_map.emplace (key, m_id);
      return r.second ? m_id++ : r.first->second;
#endif
    }
};

class Freebox :
  public P8PLATFORM::CThread
{
  protected:
    inline static unsigned int ChannelId (const std::string & uuid)
    {
      return std::stoi (uuid.substr (11)); // uuid-webtv-*
    }

    inline static unsigned int BroadcastId (const std::string & uuid)
    {
      return std::stoi (uuid.substr (6)); // pluri_*
    }

    // Channel source.
    enum class Source {DEFAULT = -1, AUTO = 0, IPTV = 1, DVB = 2};

    // Channel quality.
    enum class Quality {DEFAULT = -1, AUTO = 0, HD = 1, SD = 2, LD = 3, STEREO = 4};

    // Streaming protocol.
    enum class Protocol {DEFAULT = -1, RTSP = 1, HLS = 2};

    class Stream
    {
      public:
        enum Source  source;
        enum Quality quality;
        std::string  url;

      protected:
        int score (enum Source)  const;
        int score (enum Quality) const;

      public:
        Stream (enum Source, enum Quality, const std::string &);
        int score (enum Source, enum Quality) const;
    };

    class Channel
    {
      public:
        typedef std::map<enum Quality, std::string> Streams;

      public:
        bool                radio;
        std::string         uuid;
        std::string         name;
        std::string         logo;
        int                 major;
        int                 minor;
        std::vector<Stream> streams;

      public:
        Channel (const std::string & uuid,
                 const std::string & name,
                 const std::string & logo,
                 int major, int minor,
                 const std::vector<Stream> &);

        bool IsHidden () const;
        void GetChannel (ADDON_HANDLE, bool radio) const;
        PVR_ERROR GetStreamProperties (enum Source, enum Quality,
                                       PVR_NAMED_VALUE *, unsigned int * count) const;
    };

    // Query types.
    enum QueryType {NONE = 0, FULL = 1, CHANNEL = 2, EVENT = 3};

    class Query
    {
      public:
        QueryType    type;
        std::string  query;
        unsigned int channel;
        time_t       date;

      public:
        Query () : type (NONE) {}

        Query (QueryType t,
               const std::string & q,
               unsigned int c = 0,
               time_t d = 0) :
          type (t),
          query (q),
          channel (c),
          date (d)
        {
        }
    };

    // EPG events.
    class Event
    {
      public:
        static std::string Native (int);
        static int         Colors (int);

      public:
        class CastMember
        {
          public:
            std::string job;
            std::string first_name;
            std::string last_name;
            std::string role;

          public:
            CastMember (const rapidjson::Value &);
        };

        typedef std::vector<CastMember> Cast;

      protected:
        class ConcatIfJob
        {
          private:
            std::string m_job;

          public:
            ConcatIfJob (const std::string & job);
            std::string operator() (const std::string &, const Freebox::Event::CastMember &) const;
        };

      public:
        unsigned int channel;
        std::string  uuid;
        time_t       date;
        int          duration;
        std::string  title;
        std::string  subtitle;
        int          season;
        int          episode;
        int          category;
        std::string  picture;
        std::string  plot;
        std::string  outline;
        int          year;
        Cast         cast;

      public:
        Event (const rapidjson::Value &, unsigned int channel, time_t date);
        std::string GetCastDirector () const;
        std::string GetCastActors   () const;
    };

    // Generator.
    class Generator
    {
      public:
        int          id;
      //std::string  type;
        std::string  media;
        std::string  path;
        std::string  name;
      //std::string  subname;
        std::string  channel_uuid;
      //std::string  channel_type;
      //std::string  channel_quality;
      //bool         channel_strict;
      //std::string  broadcast_type;
        unsigned int start_hour;
        unsigned int start_min;
      //unsigned int start_sec;
        unsigned int duration;
        unsigned int margin_before;
        unsigned int margin_after;
        bool         repeat_monday;
        bool         repeat_tuesday;
        bool         repeat_wednesday;
        bool         repeat_thursday;
        bool         repeat_friday;
        bool         repeat_saturday;
        bool         repeat_sunday;

      public:
        Generator (const rapidjson::Value &);
    };

    // Timer.
    class Timer
    {
      public:
        int          id;
        time_t       start;
        time_t       end;
        unsigned int margin_before;
        unsigned int margin_after;
        std::string  name;
        std::string  subname;
        std::string  channel_uuid;
        std::string  channel_name;
      //std::string  channel_type;
      //std::string  channel_quality;
      //std::string  broadcast_type;
        std::string  media;
        std::string  path;
        bool         has_record_gen;
        int          record_gen_id;
        bool         enabled;
        bool         conflict;
        std::string  state;
        std::string  error;

      public:
        Timer (const rapidjson::Value &);
    };

    // Recording.
    class Recording
    {
      public:
        int          id;
        time_t       start;
        time_t       end;
        std::string  name;
        std::string  subname;
        std::string  channel_uuid;
        std::string  channel_name;
      //std::string  channel_type;
      //std::string  channel_quality;
      //std::string  broadcast_type;
        std::string  media;
        std::string  path;
        std::string  filename;
        bool         secure;

      public:
        Recording (const rapidjson::Value &);
    };

  public:
    Freebox (const std::string & path, const std::string & server, int source, int quality, int protocol, int days, bool extended, bool colors, int delay);
    virtual ~Freebox ();

    // Freebox Server.
    void SetServer (const std::string &);
    std::string GetServer () const;

    // Source setting.
    void SetSource (int);
    // Quality setting.
    void SetQuality (int);
    // Streaming protocol.
    void SetProtocol (int);
    // MaxDays setting.
    void SetDays (int);
    // Extended EPG.
    void SetExtended (bool);
    // Colored Categories.
    void SetColors (bool);
    // Delay setting.
    void SetDelay (int);

    // C H A N N E L S /////////////////////////////////////////////////////////
    int       GetChannelsAmount ();
    PVR_ERROR GetChannels (ADDON_HANDLE, bool radio);
    int       GetChannelGroupsAmount ();
    PVR_ERROR GetChannelGroups (ADDON_HANDLE, bool radio);
    PVR_ERROR GetChannelGroupMembers (ADDON_HANDLE, const PVR_CHANNEL_GROUP &);
    PVR_ERROR GetChannelStreamProperties (const PVR_CHANNEL *, PVR_NAMED_VALUE *, unsigned int * count);

    // R E C O R D I N G S /////////////////////////////////////////////////////
    int       GetRecordingsAmount (bool deleted) const;
    PVR_ERROR GetRecordings (ADDON_HANDLE, bool deleted) const;
    PVR_ERROR GetRecordingStreamProperties (const PVR_RECORDING *, PVR_NAMED_VALUE *, unsigned int * count) const;
    PVR_ERROR RenameRecording (const PVR_RECORDING &);
    PVR_ERROR DeleteRecording (const PVR_RECORDING &);

    // T I M E R S /////////////////////////////////////////////////////////////
    PVR_ERROR GetTimerTypes (PVR_TIMER_TYPE [], int * size) const;
    int       GetTimersAmount () const;
    PVR_ERROR GetTimers (ADDON_HANDLE) const;
    PVR_ERROR AddTimer    (const PVR_TIMER &);
    PVR_ERROR UpdateTimer (const PVR_TIMER &);
    PVR_ERROR DeleteTimer (const PVR_TIMER &, bool force);

    // M E N U / H O O K S /////////////////////////////////////////////////////
    PVR_ERROR MenuHook (const PVR_MENUHOOK &, const PVR_MENUHOOK_DATA &);

  protected:
    virtual void * Process ();

    // H T T P /////////////////////////////////////////////////////////////////
    bool Http       (const std::string & custom,
                     const std::string & url,
                     const rapidjson::Document &,
                     rapidjson::Document *, rapidjson::Type = rapidjson::kObjectType) const;
    bool HttpGet    (const std::string & url,
                     rapidjson::Document *, rapidjson::Type = rapidjson::kObjectType) const;
    bool HttpPost   (const std::string & url,
                     const rapidjson::Document &,
                     rapidjson::Document *, rapidjson::Type = rapidjson::kObjectType) const;
    bool HttpPut    (const std::string & url,
                     const rapidjson::Document &,
                     rapidjson::Document *, rapidjson::Type = rapidjson::kObjectType) const;
    bool HttpDelete (const std::string & url, rapidjson::Document *) const;

    // Session.
    bool StartSession ();
    bool CloseSession ();

    // Process JSON channels.
    bool ProcessChannels ();

    // Process JSON EPG.
    void ProcessFull    (const rapidjson::Value & epg);
    void ProcessChannel (const rapidjson::Value & epg, unsigned int channel);
    void ProcessEvent   (const rapidjson::Value & epg, unsigned int channel, time_t, EPG_EVENT_STATE);

    // If /api/v6/tv/epg/programs/* queries had a "date", things would be *way* easier!
    void ProcessEvent   (const Event &, EPG_EVENT_STATE);

    void ProcessGenerators ();
    void ProcessTimers     ();
    void ProcessRecordings ();

    // Channel preferences.
    enum Source  ChannelSource  (unsigned int id, bool fallback = true);
    enum Quality ChannelQuality (unsigned int id, bool fallback = true);
    void SetChannelSource  (unsigned int id, enum Source);
    void SetChannelQuality (unsigned int id, enum Quality);

  protected:
    static enum Source   ParseSource   (const std::string &);
    static enum Quality  ParseQuality  (const std::string &);
    static enum Protocol ParseProtocol (const std::string &);

    static std::string StrSource   (enum Source);
    static std::string StrQuality  (enum Quality);
    static std::string StrProtocol (enum Protocol);

    static enum Source  DialogSource  (enum Source  selected =  Source::DEFAULT);
    static enum Quality DialogQuality (enum Quality selected = Quality::DEFAULT);

    static std::string Password (const std::string & token, const std::string & challenge);

    template <typename T>
    inline static T JSON (const rapidjson::Value &);

    template <typename T>
    inline static T JSON (const rapidjson::Value &, const char * name, const T & value = T ());

  protected:
    // Full URL (protocol + server + query).
    std::string URL (const std::string & query) const;

  private:
    mutable P8PLATFORM::CMutex m_mutex;
    // Add-on path.
    std::string m_path;
    // Freebox Server.
    std::string m_server;
    // Delay between queries.
    int m_delay;
    // Freebox OS //////////////////////////////////////////////////////////////
    std::string m_app_token;
    int m_track_id;
    std::string m_session_token;
    // TV //////////////////////////////////////////////////////////////////////
    std::map<unsigned int, Channel> m_tv_channels;
    enum Source   m_tv_source;
    enum Quality  m_tv_quality;
    enum Protocol m_tv_protocol;
    std::map<unsigned int, enum Source>  m_tv_prefs_source;
    std::map<unsigned int, enum Quality> m_tv_prefs_quality;
    // EPG /////////////////////////////////////////////////////////////////////
    std::queue<Query> m_epg_queries;
    std::set<std::string> m_epg_cache;
    int m_epg_days;
    time_t m_epg_last;
    bool m_epg_extended;
    bool m_epg_colors;
    // Recordings //////////////////////////////////////////////////////////////
    std::map<int, Recording> m_recordings;
    // Timers //////////////////////////////////////////////////////////////////
    mutable Index<std::string> m_unique_id;
    std::map<int, Generator> m_generators;
    std::map<int, Timer> m_timers;
};

template <> inline bool        Freebox::JSON<bool>        (const rapidjson::Value & json) {return json.GetBool   ();}
template <> inline int         Freebox::JSON<int>         (const rapidjson::Value & json) {return json.GetInt    ();}
template <> inline std::string Freebox::JSON<std::string> (const rapidjson::Value & json) {return json.GetString ();}

template <typename T>
T Freebox::JSON (const rapidjson::Value & json, const char * name, const T & value)
{
  auto f = json.FindMember (name);
  return f != json.MemberEnd () ? JSON<T> (f->value) : value;
}

