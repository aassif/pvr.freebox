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
#include <nlohmann/json.hpp>
#include "kodi/addon-instance/PVR.h"
#include "kodi/tools/Thread.h"

#define PVR_FREEBOX_VERSION STR(FREEBOX_VERSION)

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

#define PVR_FREEBOX_DEFAULT_SERVER   "mafreebox.freebox.fr"
#define PVR_FREEBOX_DEFAULT_DELAY    10
#define PVR_FREEBOX_DEFAULT_SOURCE   Source::IPTV
#define PVR_FREEBOX_DEFAULT_QUALITY  Quality::HD
#define PVR_FREEBOX_DEFAULT_PROTOCOL Protocol::RTSP
#define PVR_FREEBOX_DEFAULT_EXTENDED false
#define PVR_FREEBOX_DEFAULT_COLORS   false

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

class ATTRIBUTE_HIDDEN Freebox :
  public kodi::addon::CAddonBase,
  public kodi::addon::CInstancePVRClient,
  public kodi::tools::CThread
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
        std::string  rtsp;
        std::string  hls;

      protected:
        int score (enum Source)  const;
        int score (enum Quality) const;

      public:
        Stream (enum Source,
                enum Quality,
                const std::string & rtsp,
                const std::string & hls);
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
        void GetChannel (kodi::addon::PVRChannelsResultSet & results, bool radio) const;
        PVR_ERROR GetStreamProperties (enum Source, enum Quality, enum Protocol,
                                       std::vector<kodi::addon::PVRStreamProperty> & properties) const;
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
            CastMember (const nlohmann::json &);
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
        Event (const nlohmann::json &, unsigned int channel, time_t date);
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
        Generator (const nlohmann::json &);
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
        Timer (const nlohmann::json &);
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
        int          byte_size;
        bool         secure;

      public:
        Recording (const nlohmann::json &);
    };

  public:
    Freebox ();
    virtual ~Freebox ();

    // A D D O N  - B A S I C S ////////////////////////////////////////////////
    ADDON_STATUS Create () override;
    ADDON_STATUS SetSetting (const std::string &, const kodi::CSettingValue &) override;

    // P V R  - B A S I C S ////////////////////////////////////////////////////
    PVR_ERROR GetCapabilities (kodi::addon::PVRCapabilities &) override;
    PVR_ERROR GetBackendName (std::string &) override;
    PVR_ERROR GetBackendVersion (std::string &) override;
    PVR_ERROR GetBackendHostname (std::string &) override;
    PVR_ERROR GetConnectionString (std::string &) override;

    // E P G ///////////////////////////////////////////////////////////////////
    PVR_ERROR SetEPGMaxPastDays(int) override;
    PVR_ERROR SetEPGMaxFutureDays(int) override;
    PVR_ERROR GetEPGForChannel(int, time_t, time_t, kodi::addon::PVREPGTagsResultSet &) override;

    // C H A N N E L S /////////////////////////////////////////////////////////
    PVR_ERROR GetChannelsAmount(int &) override;
    PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet &) override;
    PVR_ERROR GetChannelGroupsAmount(int &) override;
    PVR_ERROR GetChannelGroups(bool, kodi::addon::PVRChannelGroupsResultSet &) override;
    PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup &, kodi::addon::PVRChannelGroupMembersResultSet &) override;
    PVR_ERROR GetChannelStreamProperties(const kodi::addon::PVRChannel &, std::vector<kodi::addon::PVRStreamProperty> &) override;

    // R E C O R D I N G S /////////////////////////////////////////////////////
    PVR_ERROR GetRecordingsAmount(bool, int &) override;
    PVR_ERROR GetRecordings(bool, kodi::addon::PVRRecordingsResultSet &) override;
    PVR_ERROR GetRecordingSize (const kodi::addon::PVRRecording &, int64_t &) override;
    PVR_ERROR GetRecordingStreamProperties(const kodi::addon::PVRRecording &, std::vector<kodi::addon::PVRStreamProperty> &) override;
    PVR_ERROR RenameRecording (const kodi::addon::PVRRecording &) override;
    PVR_ERROR DeleteRecording (const kodi::addon::PVRRecording &) override;

    // T I M E R S /////////////////////////////////////////////////////////////
    PVR_ERROR GetTimerTypes( std::vector<kodi::addon::PVRTimerType> &) override;
    PVR_ERROR GetTimersAmount (int &) override;
    PVR_ERROR GetTimers   (kodi::addon::PVRTimersResultSet &) override;
    PVR_ERROR AddTimer    (const kodi::addon::PVRTimer &) override;
    PVR_ERROR UpdateTimer (const kodi::addon::PVRTimer &) override;
    PVR_ERROR DeleteTimer (const kodi::addon::PVRTimer &, bool) override;

    // M E N U / H O O K S /////////////////////////////////////////////////////
    PVR_ERROR CallChannelMenuHook (const kodi::addon::PVRMenuhook &, const kodi::addon::PVRChannel &) override;

  protected:
    void Process () override;

    void ReadSettings ();

    // Freebox Server.
    void SetServer (const std::string &);
    std::string GetServer () const;

    // Source setting.
    void SetSource (Source);
    // Quality setting.
    void SetQuality (Quality);
    // Streaming protocol.
    void SetProtocol (Protocol);
    // Maximum past days.
    void SetPastDays (int);
    // Maximum future days.
    void SetFutureDays (int);
    // Extended EPG.
    void SetExtended (bool);
    // Colored Categories.
    void SetColors (bool);
    // Delay setting.
    void SetDelay (int);

    // H T T P /////////////////////////////////////////////////////////////////
    bool Http       (const std::string & custom,
                     const std::string & url,
                     const nlohmann::json &,
                     nlohmann::json *,
                     nlohmann::json::value_t = nlohmann::json::value_t::object) const;
    bool HttpGet    (const std::string & url,
                     nlohmann::json *,
                     nlohmann::json::value_t = nlohmann::json::value_t::object) const;
    bool HttpPost   (const std::string & url,
                     const nlohmann::json &,
                     nlohmann::json *,
                     nlohmann::json::value_t = nlohmann::json::value_t::object) const;
    bool HttpPut    (const std::string & url,
                     const nlohmann::json &,
                     nlohmann::json *,
                     nlohmann::json::value_t = nlohmann::json::value_t::object) const;
    bool HttpDelete (const std::string & url) const;

    // Session.
    bool StartSession ();
    bool CloseSession ();

    // Process JSON channels.
    bool ProcessChannels ();

    // Process JSON EPG.
    void ProcessFull    (const nlohmann::json & epg);
    void ProcessChannel (const nlohmann::json & epg, unsigned int channel);
    void ProcessEvent   (const nlohmann::json & epg, unsigned int channel, time_t, EPG_EVENT_STATE);

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

  protected:
    // Full URL (protocol + server + query).
    std::string URL (const std::string & query) const;

  private:
    mutable std::recursive_mutex m_mutex;
    // Add-on path.
    std::string m_path;
    // Freebox Server.
    std::string m_server = PVR_FREEBOX_DEFAULT_SERVER;
    // Delay between queries.
    int m_delay = PVR_FREEBOX_DEFAULT_DELAY;
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
    int m_epg_days_past;
    int m_epg_days_future;
    time_t m_epg_last;
    bool m_epg_extended = PVR_FREEBOX_DEFAULT_EXTENDED;
    bool m_epg_colors   = PVR_FREEBOX_DEFAULT_COLORS;
    // Recordings //////////////////////////////////////////////////////////////
    std::map<int, Recording> m_recordings;
    // Timers //////////////////////////////////////////////////////////////////
    mutable Index<std::string> m_unique_id;
    std::map<int, Generator> m_generators;
    std::map<int, Timer> m_timers;
};

