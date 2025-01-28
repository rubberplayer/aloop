
/*
 * xui.h
 *
 * SPDX-License-Identifier:  BSD-3-Clause
 *
 * Copyright (C) 2024 brummer <brummer@web.de>
 */

#ifndef JACKAPI
#include <portaudio.h>
#endif
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <vector>
#include <string>
#include <sndfile.hh>
#include <fstream>

#include "CheckResample.h"

#include "xwidgets.h"
#include "xfile-dialog.h"
#include "TextEntry.h"

#pragma once

#ifndef AUDIOLOOPERUI_H
#define AUDIOLOOPERUI_H

class SupportedFormats {
public:
    SupportedFormats() {
        supportedExtensions = getSupportedFileExtensions();
    }

    bool isSupported(std::string filename) {
        std::filesystem::path p(filename);
        std::string ext = p.extension().string();

        if (not ext.empty()) {
            // check for lower-cased file extension
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
            return supportedExtensions.count(ext.substr(1)) >= 1;
        }

        return false;
    }

private:
    std::set<std::string> supportedExtensions;

    std::set<std::string> getSupportedFileExtensions() {
        std::set<std::string> extensions;

        // Get the number of supported simple formats
        int simpleFormatCount;
        sf_command(SF_NULL, SFC_GET_SIMPLE_FORMAT_COUNT, &simpleFormatCount, sizeof(int));

        // Get the number of supported major formats
        int majorFormatCount;
        sf_command(SF_NULL, SFC_GET_FORMAT_MAJOR_COUNT, &majorFormatCount, sizeof(int));

        // Get the number of supported sub formats
        int subFormatCount;
        sf_command(SF_NULL, SFC_GET_FORMAT_SUBTYPE_COUNT, &subFormatCount, sizeof(int));

        // Get information about each simple format
        for (int i = 0; i < simpleFormatCount; ++i) {
            SF_FORMAT_INFO formatInfo;
            formatInfo.format = i;
            sf_command(SF_NULL, SFC_GET_SIMPLE_FORMAT, &formatInfo, sizeof(formatInfo));

            if (formatInfo.extension != SF_NULL)
                extensions.insert(formatInfo.extension);
        }

        // Get information about each major format
        for (int i = 0; i < majorFormatCount; i++) {
            SF_FORMAT_INFO formatInfo;
            formatInfo.format = i;
            sf_command(SF_NULL, SFC_GET_FORMAT_MAJOR, &formatInfo, sizeof(formatInfo));

            if (formatInfo.extension != SF_NULL)
                extensions.insert(formatInfo.extension);
        }

        // Get information about each sub format
        for (int j = 0; j < subFormatCount; j++) {
            SF_FORMAT_INFO formatInfo;
            formatInfo.format = j;
            sf_command(SF_NULL, SFC_GET_FORMAT_SUBTYPE, &formatInfo, sizeof(SF_FORMAT_INFO));

            if (formatInfo.extension != SF_NULL)
                extensions.insert(formatInfo.extension);
        }

        return extensions;
    }
};

class AudioLooperUi: CheckResample, public TextEntry
{
public:
    Widget_t *w;
    ParallelThread pa;
    ParallelThread pl;

    float *samples;
    uint32_t channels;
    uint32_t samplesize;
    uint32_t samplerate;
    uint32_t jack_sr;
    uint32_t position;
    float gain;
    bool loadNew;
    bool play;
    bool ready;
    bool playBackwards;

    AudioLooperUi() {
        samplesize = 0;
        samplerate = 0;
        jack_sr = 0;
        position = 0;
        playNow = 0;
        gain = std::pow(1e+01, 0.05 * 0.0);
        samples = nullptr;
        loadNew = false;
        play = true;
        ready = true;
        usePlayList = false;
        forceReload = false;
        playBackwards = false;
        viewPlayList = nullptr;
        stream = nullptr;
        if (getenv("XDG_CONFIG_HOME")) {
            std::string path = getenv("XDG_CONFIG_HOME");
            config_file = path + "/alooper.conf";
        } else {
            std::string path = getenv("HOME");
            config_file = path +"/.config/alooper.conf";
        }
        readPlayList();
    };

    ~AudioLooperUi() {
        delete[] samples;
        pl.stop();
        pa.stop();
        PlayList.clear();
        PlayListNames.clear();
    };

/****************************************************************
                      public function calls
****************************************************************/

    // stop background threads and quit main window
    void onExit() {
        pl.stop();
        pa.stop();
        quit(w);
    }

    // receive Sample Rate from audio back-end
    void setJackSampleRate(uint32_t sr) {
        jack_sr = sr;
    }

    // receive stream object from portaudio to check 
    // if the server is actual running
    void setPaStream(PaStream* stream_) {
        stream = stream_;
    }

    // receive a file name from the File Browser or the command-line
    static void dialog_response(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if (!Pa_IsStreamActive(self->stream)) return;
        if(user_data !=NULL) {
            self->read_soundfile(*(const char**)user_data);
            self->addToPlayList(*(char**)user_data, true);
        } else {
            fprintf(stderr, "no file selected\n");
        }
    }

/****************************************************************
                      main window
****************************************************************/

    // create the main GUI
    void createGUI(Xputty *app, std::condition_variable *Sync_) {
        SyncWait =Sync_;
        w = create_window(app, os_get_root_window(app, IS_WINDOW), 0, 0, 400, 170);
        widget_set_title(w, "alooper");
        widget_set_icon_from_png(w,LDVAR(alooper_png));
        widget_set_dnd_aware(w);
        w->parent_struct = (void*)this;
        w->func.expose_callback = draw_window;
        w->func.dnd_notify_callback = dnd_load_response;

        wview = add_waveview(w, "", 20, 20, 360, 100);
        wview->scale.gravity = NORTHWEST;
        wview->parent_struct = (void*)this;
        wview->adj_x = add_adjustment(wview,0.0, 0.0, 0.0, 1000.0,1.0, CL_METER);
        wview->adj = wview->adj_x;
        wview->func.expose_callback = draw_wview;
        wview->func.button_release_callback = set_playhead;

        filebutton = add_file_button(w, 20, 130, 30, 30, getenv("HOME") ? getenv("HOME") : "/", "audio");
        filebutton->scale.gravity = SOUTHEAST;
        filebutton->parent_struct = (void*)this;
        widget_get_png(filebutton, LDVAR(dir_png));
        filebutton->func.user_callback = dialog_response;

        lview = add_image_toggle_button(w, "", 60, 130, 30, 30);
        lview->parent_struct = (void*)this;
        lview->scale.gravity = SOUTHEAST;
        widget_get_png(lview, LDVAR(menu_png));
        lview->func.value_changed_callback = button_lview_callback;

        volume = add_knob(w, "dB",220,130,28,28);
        volume->parent_struct = (void*)this;
        volume->scale.gravity = SOUTHWEST;
        set_adjustment(volume->adj, 0.0, 0.0, -20.0, 6.0, 0.1, CL_CONTINUOS);
        volume->func.expose_callback = draw_knob;
        volume->func.value_changed_callback = volume_callback;

        backwards = add_image_toggle_button(w, "", 260, 130, 30, 30);
        backwards->scale.gravity = SOUTHWEST;
        backwards->parent_struct = (void*)this;
        widget_get_png(backwards, LDVAR(backwards_png));
        backwards->func.value_changed_callback = button_backwards_callback;

        backset = add_button(w, "", 290, 130, 30, 30);
        backset->parent_struct = (void*)this;
        backset->scale.gravity = SOUTHWEST;
        widget_get_png(backset, LDVAR(rewind_png));
        backset->func.value_changed_callback = button_backset_callback;

        paus = add_image_toggle_button(w, "", 320, 130, 30, 30);
        paus->scale.gravity = SOUTHWEST;
        paus->parent_struct = (void*)this;
        widget_get_png(paus, LDVAR(pause_png));
        paus->func.value_changed_callback = button_pause_callback;

        w_quit = add_button(w, "", 350, 130, 30, 30);
        w_quit->parent_struct = (void*)this;
        widget_get_png(w_quit, LDVAR(exit__png));
        w_quit->scale.gravity = SOUTHWEST;
        w_quit->func.value_changed_callback = button_quit_callback;

        widget_show_all(w);

        pa.startTimeout(60);
        pa.set<AudioLooperUi, &AudioLooperUi::updateUI>(this);

        pl.start();
        pl.set<AudioLooperUi, &AudioLooperUi::loadFromPlayList>(this);
    }

private:
    Widget_t *w_quit;
    Widget_t *filebutton;
    Widget_t *wview;
    Widget_t *paus;
    Widget_t *backset;
    Widget_t *volume;
    Widget_t *backwards;
    Widget_t *lview;
    Widget_t *viewPlayList;
    Widget_t *playList;
    Widget_t *deleteEntry;
    Widget_t *upEntry;
    Widget_t *downEntry;
    Widget_t *loadPlayList;
    Widget_t *savePlayList;
    Widget_t *LoadMenu;
    std::condition_variable *SyncWait;
    std::mutex WMutex;
    SupportedFormats supportedFormats;
    PaStream* stream;
    std::vector<std::tuple< std::string, std::string> > PlayList;
    std::vector<std::tuple< std::string, std::string> >::iterator lfile;
    std::vector<std::string> PlayListNames;
    uint32_t playNow;
    bool usePlayList;
    bool forceReload;
    std::string config_file;

/****************************************************************
            PlayList - create the window
****************************************************************/

    // create the Play List window
    void createPlayListView(Xputty *app) {
        viewPlayList = create_window(app, os_get_root_window(app, IS_WINDOW), 0, 0, 400, 340);
        viewPlayList->flags |= HIDE_ON_DELETE;
        widget_set_title(viewPlayList, "alooper-Playlist");
        widget_set_icon_from_png(viewPlayList,LDVAR(alooper_png));
        widget_set_dnd_aware(viewPlayList);
        viewPlayList->parent_struct = (void*)this;
        viewPlayList->func.expose_callback = draw_window;
        viewPlayList->func.dnd_notify_callback = dnd_load_playlist;

        playList = add_listbox(viewPlayList, "", 20, 20, 360, 270);
        playList->parent_struct = (void*)this;
        playList->scale.gravity = NORTHWEST;

        loadPlayList = add_button(viewPlayList, "", 20, 300, 30, 30);
        loadPlayList->parent_struct = (void*)this;
        widget_get_png(loadPlayList, LDVAR(load__png));
        loadPlayList->scale.gravity = SOUTHEAST;
        loadPlayList->func.value_changed_callback = load_up_callback;

        savePlayList = add_button(viewPlayList, "", 50, 300, 30, 30);
        savePlayList->parent_struct = (void*)this;
        widget_get_png(savePlayList, LDVAR(save__png));
        savePlayList->scale.gravity = SOUTHEAST;
        savePlayList->func.value_changed_callback = save_as_callback;

        upEntry = add_button(viewPlayList, "", 320, 300, 30, 30);
        upEntry->parent_struct = (void*)this;
        widget_get_png(upEntry, LDVAR(up_png));
        upEntry->scale.gravity = SOUTHWEST;
        upEntry->func.value_changed_callback = up_entry_callback;

        downEntry = add_button(viewPlayList, "", 350, 300, 30, 30);
        downEntry->parent_struct = (void*)this;
        widget_get_png(downEntry, LDVAR(down_png));
        downEntry->scale.gravity = SOUTHWEST;
        downEntry->func.value_changed_callback = down_entry_callback;

        deleteEntry = add_button(viewPlayList, "", 290, 300, 30, 30);
        deleteEntry->parent_struct = (void*)this;
        widget_get_png(deleteEntry, LDVAR(quit_png));
        deleteEntry->scale.gravity = SOUTHWEST;
        deleteEntry->func.value_changed_callback = remove_entry_callback;

        LoadMenu = create_menu(loadPlayList,25);
        LoadMenu->parent_struct = (void*)this;
        LoadMenu->func.button_release_callback = load_playlist_callback;
    }

/****************************************************************
            PlayList - callbacks
****************************************************************/

    // load next file from Play List, called from background thread,
    // triggered by audio server when end of current file is reached
    void loadFromPlayList() {
        if (((PlayList.size() < 2) || !usePlayList) && !forceReload) return;
        forceReload = false;
        playNow++;
        lfile = PlayList.begin()+playNow;
        if (lfile >= PlayList.end()) {
            lfile = PlayList.begin();
            playNow = 0;
        }
        #if defined(__linux__) || defined(__FreeBSD__) || \
            defined(__NetBSD__) || defined(__OpenBSD__)
        XLockDisplay(w->app->dpy);
        #endif
        listbox_set_active_entry(playList, playNow);
        #if defined(__linux__) || defined(__FreeBSD__) || \
            defined(__NetBSD__) || defined(__OpenBSD__)
        XFlush(w->app->dpy);
        XUnlockDisplay(w->app->dpy);
        #endif

        read_soundfile(std::get<1>(*lfile).c_str());
    }

    // add a file to the Play List
    void addToPlayList(void* fileName, bool laod) {
        PlayList.push_back(std::tuple<std::string, std::string>(
            std::string(basename((char*)fileName)), std::string((const char*)fileName)));
        #if defined(__linux__) || defined(__FreeBSD__) || \
            defined(__NetBSD__) || defined(__OpenBSD__)
        XLockDisplay(w->app->dpy);
        #endif
        auto it = PlayList.end()-1;
        listbox_add_entry(playList, std::get<0>(*it).c_str());
        if (laod) playNow = PlayList.size()-1;
        Metrics_t metrics;
        os_get_window_metrics(playList, &metrics);
        if (metrics.visible) {
            if (laod) listbox_set_active_entry(playList, playNow);
            widget_show_all(playList);
        }
        #if defined(__linux__) || defined(__FreeBSD__) || \
            defined(__NetBSD__) || defined(__OpenBSD__)
        XFlush(w->app->dpy);
        XUnlockDisplay(w->app->dpy);
        #endif
    }

    // handle drag and drop for the Play List window
    static void dnd_load_playlist(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if (!Pa_IsStreamActive(self->stream)) return;
        if (user_data != NULL) {
            char* dndfile = NULL;
            dndfile = strtok(*(char**)user_data, "\r\n");
            while (dndfile != NULL) {
                if (self->supportedFormats.isSupported(dndfile) ) {
                    if (!self->PlayList.size()) self->read_soundfile(dndfile);
                    self->addToPlayList(dndfile, false);
                    self->forceReload = true;
                } else {
                    std::cerr << "Unrecognized file extension: " << dndfile << std::endl;
                }
                dndfile = strtok(NULL, "\r\n");
            }
        }
    }

    // re-build the Play List when files was re-moved
    void rebuildPlayList() {
        listbox_remove_entrys(playList);
        for (auto it = PlayList.begin(); it != PlayList.end(); it++)
            listbox_add_entry(playList, std::get<0>(*it).c_str());
        listbox_set_active_entry(playList, playNow);
        Metrics_t metrics;
        os_get_window_metrics(playList, &metrics);
        if (metrics.visible) widget_show_all(viewPlayList);
    }

    // remove a entry from the Play List
    static void remove_entry_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if ((w->flags & HAS_POINTER) && adj_get_value(w->adj)){
            if (!self->PlayList.size()) return;
            int remove = static_cast<int>(adj_get_value(self->playList->adj));
            self->PlayList.erase(self->PlayList.begin() + remove);
            self->rebuildPlayList();
            self->forceReload = true;
        }
    }

    // move a entry one place up in the Play List
    static void up_entry_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if ((w->flags & HAS_POINTER) && adj_get_value(w->adj)){
            if (!self->PlayList.size()) return;
            int up = static_cast<int>(adj_get_value(self->playList->adj));
            if (!up) return;
            std::swap(self->PlayList[up-1],self->PlayList[up]);
            self->rebuildPlayList();
        }
    }

    // move a entry one place down in the Play List
    static void down_entry_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if ((w->flags & HAS_POINTER) && adj_get_value(w->adj)){
            if (!self->PlayList.size()) return;
            int down = static_cast<int>(adj_get_value(self->playList->adj));
            if (down > static_cast<int>(self->PlayList.size()-1)) return;
            std::swap(self->PlayList[down],self->PlayList[down+1]);
            self->rebuildPlayList();
        }
    }

    // load a saved Play List by a given name
    static void load_playlist_callback(void* w_, void* item_, void* data_) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        int v = *(int*)item_;
        self->PlayList.clear();
        self->load_PlayList(self->PlayListNames[v].c_str());
        self->rebuildPlayList();
        if (!self->samples) {
            auto i = self->PlayList.begin();
            self->read_soundfile(std::get<1>(*i).c_str());
        } else {
            self->playNow = self->PlayList.size()-1;
        }
    }

    // pop up a menu to select the Play List to load
    static void load_up_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if ((w->flags & HAS_POINTER) && !adj_get_value(w->adj)){
            self->PlayListNames.clear();
            self->readPlayList();

            Widget_t *view_port = self->LoadMenu->childlist->childs[0];
            int i = view_port->childlist->elem;
            for(;i>-1;i--) {
                menu_remove_item(self->LoadMenu,view_port->childlist->childs[i]);
            }

            for (auto i = self->PlayListNames.begin(); i != self->PlayListNames.end(); i++) {
                menu_add_item(self->LoadMenu, (*i).c_str());
            }
            pop_menu_show(w, self->LoadMenu, 6, true);
        }
    }

    // save a Play List under the given name
    static void save_response(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        if(user_data !=NULL && strlen(*(const char**)user_data)) {
            AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
            std::string lname(*(const char**)user_data);
            if (std::find(self->PlayListNames.begin(), self->PlayListNames.end(), lname) 
                                                            != self->PlayListNames.end()) {
                Widget_t* dia = self->showTextEntry(self->viewPlayList, 
                            "Playlist - name already exists:", "Choose a other name:");
                int x1, y1;
                os_translate_coords( self->viewPlayList, self->viewPlayList->widget, 
                    os_get_root_window(self->w->app, IS_WIDGET), 0, 0, &x1, &y1);
                os_move_window(self->w->app->dpy,dia,x1+60, y1+16);
                self->viewPlayList->func.dialog_callback = save_response;
            } else {
                self->save_PlayList(lname, true);
            }
        }
    }

    // pop up a text entry to enter a name for a Play List to save
    static void save_as_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if ((w->flags & HAS_POINTER) && !adj_get_value(w->adj)){
            if (!self->PlayList.size()) return;
            Widget_t* dia = self->showTextEntry(self->viewPlayList, 
                        "Playlist - save as:", "Save Play List as:");
            int x1, y1;
            os_translate_coords( self->viewPlayList, self->viewPlayList->widget, 
                os_get_root_window(self->w->app, IS_WIDGET), 0, 0, &x1, &y1);
            os_move_window(self->w->app->dpy,dia,x1+60, y1+16);
            self->viewPlayList->func.dialog_callback = save_response;
        }
    }


/****************************************************************
                Read/Save/Load a Play List
****************************************************************/

    // remove key from line
    std::string remove_sub(std::string a, std::string b) {
        std::string::size_type fpos = a.find(b);
        if (fpos != std::string::npos )
            a.erase(a.begin() + fpos, a.begin() + fpos + b.length());
        return (a);
    }

    // save a Play List to file
    void save_PlayList(std::string lname, bool append) {
        std::ofstream outfile(config_file, append ? std::ios::app : std::ios::trunc);
        if (outfile.is_open()) {
             outfile << "[PlayList] "<< lname << std::endl;
             for (auto i = PlayList.begin(); i != PlayList.end(); i++) {
                outfile << "[File] "<< std::get<1>(*i) << std::endl;
             }
         }
         outfile.close();
    }

    // load a Play List by given name
    void load_PlayList(std::string LoadName) {
        std::ifstream infile(config_file);
        std::string line;
        std::string key;
        std::string value;
        std::string ListName;
        if (infile.is_open()) {
            while (std::getline(infile, line)) {
                std::istringstream buf(line);
                buf >> key;
                buf >> value;
                if (key.compare("[PlayList]") == 0) ListName = remove_sub(line, "[PlayList] ");
                if (ListName.compare(LoadName) == 0) {
                    if (key.compare("[File]") == 0) {
                        std::string fileName = remove_sub(line, "[File] ");
                        PlayList.push_back(std::tuple<std::string, std::string>(
                            std::string(basename((char*)fileName.c_str())), fileName));
                    }
                }
                key.clear();
                value.clear();
            }
        }
        infile.close();
    }

    // get the Play List names form file
    void readPlayList() {
        std::ifstream infile(config_file);
        std::string line;
        std::string key;
        std::string value;
        std::string ListName;
        if (infile.is_open()) {
            while (std::getline(infile, line)) {
                std::istringstream buf(line);
                buf >> key;
                buf >> value;
                if (key.compare("[PlayList]") == 0) PlayListNames.push_back(remove_sub(line, "[PlayList] "));
                key.clear();
                value.clear();
            }
        }
        infile.close();   
    }

/****************************************************************
                      File loading
****************************************************************/

    // when Sound File loading fail, clear wave view and reset tittle
    void failToLoad() {
        #if defined(__linux__) || defined(__FreeBSD__) || \
            defined(__NetBSD__) || defined(__OpenBSD__)
        XLockDisplay(w->app->dpy);
        #endif
        loadNew = true;
        update_waveview(wview, samples, samplesize);
        widget_set_title(w, "alooper");
        #if defined(__linux__) || defined(__FreeBSD__) || \
            defined(__NetBSD__) || defined(__OpenBSD__)
        XFlush(w->app->dpy);
        XUnlockDisplay(w->app->dpy);
        #endif
    }

    // load Sound File data into memory
    void read_soundfile(const char* file) {
        // struct to hols sound file info
        SF_INFO info;
        info.format = 0;

        channels = 0;
        samplesize = 0;
        samplerate = 0;
        position = 0;
        std::unique_lock<std::mutex> lk(WMutex);
        SyncWait->wait(lk);
        ready = false;
        delete[] samples;
        samples = nullptr;

        // Open the wave file for reading
        SNDFILE *sndfile = sf_open(file, SFM_READ, &info);

        if (!sndfile) {
            std::cerr << "Error: could not open file " << sf_error (sndfile) << std::endl;
            failToLoad();
            return ;
        }
        if (info.channels > 2) {
            std::cerr << "Error: only two channels maximum are supported!" << std::endl;
            failToLoad();
            return ;
        }
        try {
            samples = new float[info.frames * info.channels];
        } catch (...) {
            std::cerr << "Error: could not load file" << std::endl;
            failToLoad();
            return;
        }
        memset(samples, 0, info.frames * info.channels * sizeof(float));
        samplesize = (uint32_t) sf_readf_float(sndfile, &samples[0], info.frames);
        if (!samplesize ) samplesize = info.frames;
        channels = info.channels;
        samplerate = info.samplerate;
        position = 0;
        sf_close(sndfile);
        samples = checkSampleRate(&samplesize, channels, samples, samplerate, jack_sr);
        loadNew = true;
        #if defined(__linux__) || defined(__FreeBSD__) || \
            defined(__NetBSD__) || defined(__OpenBSD__)
        XLockDisplay(w->app->dpy);
        #endif
        if (samples) {
            adj_set_max_value(wview->adj, (float)samplesize);
            update_waveview(wview, samples, samplesize);
            char name[256];
            strncpy(name, file, 255);
            widget_set_title(w, basename(name));
            #if defined(__linux__) || defined(__FreeBSD__) || \
                defined(__NetBSD__) || defined(__OpenBSD__)
            XFlush(w->app->dpy);
            XUnlockDisplay(w->app->dpy);
            #endif
        } else {
            samplesize = 0;
            std::cerr << "Error: could not resample file" << std::endl;
            failToLoad();
        }
        if (playBackwards) position = samplesize;
        ready = true;
    }

/****************************************************************
            drag and drop handling for the main window
****************************************************************/

    static void dnd_load_response(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if (!Pa_IsStreamActive(self->stream)) return;
        if (user_data != NULL) {
            char* dndfile = NULL;
            dndfile = strtok(*(char**)user_data, "\r\n");
            while (dndfile != NULL) {
                if (self->supportedFormats.isSupported(dndfile) ) {
                    self->read_soundfile(dndfile);
                    self->addToPlayList(dndfile, true);
                    break;
                } else {
                    std::cerr << "Unrecognized file extension: " << dndfile << std::endl;
                }
                dndfile = strtok(NULL, "\r\n");
            }
        }
    }

/****************************************************************
            Play head (called from timeout thread) 
****************************************************************/

    static void dummy_callback(void *w_, void* user_data) {}

    void updateUI() {
        #if defined(__linux__) || defined(__FreeBSD__) || \
            defined(__NetBSD__) || defined(__OpenBSD__)
        XLockDisplay(w->app->dpy);
        wview->func.adj_callback = dummy_callback;
        #endif
        adj_set_value(wview->adj, (float) position);
        #if defined(__linux__) || defined(__FreeBSD__) || \
            defined(__NetBSD__) || defined(__OpenBSD__)
        expose_widget(wview);
        XFlush(w->app->dpy);
        wview->func.adj_callback = transparent_draw;
        XUnlockDisplay(w->app->dpy);
        #endif
    }

/****************************************************************
                      Button callbacks 
****************************************************************/

    // quit
    static void button_quit_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if (w->flags & HAS_POINTER && !*(int*)user_data){
            self->onExit();
        }
    }

    // pause
    static void button_pause_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if ((w->flags & HAS_POINTER) && adj_get_value(w->adj)){
            self->play = false;
        } else self->play = true;
    }

    // play backwards
    static void button_backwards_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if ((w->flags & HAS_POINTER) && adj_get_value(w->adj)){
            self->playBackwards = true;
        } else self->playBackwards = false;
    }

    // move playhead to start position 
    static void button_backset_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if ((w->flags & HAS_POINTER) && !adj_get_value(w->adj)){
            self->position = 0;
        }
    }

    // set playhead position to mouse pointer
    static void set_playhead(void *w_, void* xbutton_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        XButtonEvent *xbutton = (XButtonEvent*)xbutton_;
        if (w->flags & HAS_POINTER) {
            if(xbutton->state == Button1Mask) {
                Metrics_t metrics;
                os_get_window_metrics(w, &metrics);
                int width = metrics.width;
                int x = xbutton->x;
                float st = max(0.0, min(1.0, static_cast<float>((float)x/(float)width)));
                self->position = adj_get_max_value(w->adj) * st;
            }
        }
    }

    // show the Play List window
    static void button_lview_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        if ((w->flags & HAS_POINTER) && adj_get_value(w->adj)){
            if (!self->viewPlayList) self->createPlayListView(self->w->app);
            int x1, y1;
            os_translate_coords( self->w, self->w->widget, 
                os_get_root_window(self->w->app, IS_WIDGET), 0, 0, &x1, &y1);
            widget_show_all(self->viewPlayList);
            os_move_window(self->w->app->dpy,self->viewPlayList,x1, y1+16+self->w->height);
            self->usePlayList = true;
        } else {
            widget_hide(self->viewPlayList);
            self->usePlayList = false;
        }
    }

    // volume control
    static void volume_callback(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        self->gain = std::pow(1e+01, 0.05 * adj_get_value(w->adj));
    }

/****************************************************************
                      drawings 
****************************************************************/

    void roundrec(cairo_t *cr, float x, float y, float width, float height, float r) {
        cairo_arc(cr, x+r, y+r, r, M_PI, 3*M_PI/2);
        cairo_arc(cr, x+width-r, y+r, r, 3*M_PI/2, 0);
        cairo_arc(cr, x+width-r, y+height-r, r, 0, M_PI/2);
        cairo_arc(cr, x+r, y+height-r, r, M_PI/2, M_PI);
        cairo_close_path(cr);
    }

    static void draw_knob(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        Metrics_t metrics;
        os_get_window_metrics(w, &metrics);
        int width = metrics.width;
        int height = metrics.height;
        if (!metrics.visible) return;

        const double scale_zero = 20 * (M_PI/180); // defines "dead zone" for knobs
        int arc_offset = 0;
        int knob_x = 0;
        int knob_y = 0;

        int grow = (width > height) ? height:width;
        knob_x = grow-1;
        knob_y = grow-1;
        /** get values for the knob **/

        const int knobx1 = width* 0.5;

        const int knoby1 = height * 0.5;

        const double knobstate = adj_get_state(w->adj_y);
        const double angle = scale_zero + knobstate * 2 * (M_PI - scale_zero);

        const double pointer_off =knob_x/6;
        const double radius = min(knob_x-pointer_off, knob_y-pointer_off) / 2;

        const double add_angle = 90 * (M_PI / 180.);
        // base
        use_base_color_scheme(w, INSENSITIVE_);
        cairo_set_line_width(w->crb,  5.0/w->scale.ascale);
        cairo_arc (w->crb, knobx1+arc_offset, knoby1+arc_offset, radius,
              add_angle + scale_zero, add_angle + scale_zero + 320 * (M_PI/180));
        cairo_stroke(w->crb);

        // indicator
        cairo_set_line_width(w->crb,  3.0/w->scale.ascale);
        cairo_new_sub_path(w->crb);
        cairo_set_source_rgba(w->crb, 0.75, 0.75, 0.75, 1);
        cairo_arc (w->crb,knobx1+arc_offset, knoby1+arc_offset, radius,
              add_angle + scale_zero, add_angle + angle);
        cairo_stroke(w->crb);

        use_text_color_scheme(w, get_color_state(w));
        cairo_text_extents_t extents;
        /** show value on the kob**/
        char s[64];
        float value = adj_get_value(w->adj);
        snprintf(s, 63, "%.1f", value);
        cairo_set_font_size (w->crb, (w->app->small_font-2)/w->scale.ascale);
        cairo_text_extents(w->crb, s, &extents);
        cairo_move_to (w->crb, knobx1-extents.width/2, knoby1+extents.height/2);
        cairo_show_text(w->crb, s);
        cairo_new_path (w->crb);
    }

    void create_waveview_image(Widget_t *w, int width, int height) {
        cairo_surface_destroy(w->image);
        w->image = NULL;
        w->image = cairo_surface_create_similar (w->surface,
                            CAIRO_CONTENT_COLOR_ALPHA, width, height);
        cairo_t *cri = cairo_create (w->image);

        WaveView_t *wave_view = (WaveView_t*)w->private_struct;
        int half_height_t = height/2;

        cairo_set_line_width(cri,2);
        cairo_set_source_rgba(cri, 0.05, 0.05, 0.05, 1);
        roundrec(cri, 0, 0, width, height, 5);
        cairo_fill_preserve(cri);
        cairo_set_source_rgba(cri, 0.33, 0.33, 0.33, 1);
        cairo_stroke(cri);
        cairo_move_to(cri,2,half_height_t);
        cairo_line_to(cri, width, half_height_t);
        cairo_stroke(cri);

        if (wave_view->size<1) return;
        int step = (wave_view->size/width)/channels;
        float lstep = (float)(half_height_t)/channels;
        cairo_set_line_width(cri,2);
        cairo_set_source_rgba(cri, 0.55, 0.65, 0.55, 1);

        int pos = half_height_t/channels;
        for (int c = 0; c < (int)channels; c++) {
            for (int i=0;i<width-4;i++) {
                cairo_move_to(cri,i+2,pos);
                float w = wave_view->wave[int(c+(i*channels)*step)];
                cairo_line_to(cri, i+2,(float)(pos)+ (-w * lstep));
                cairo_line_to(cri, i+2,(float)(pos)+ (w * lstep));
            }
            pos += half_height_t;
        }
        cairo_stroke(cri);
        cairo_destroy(cri);
    }

    static void draw_wview(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        Metrics_t metrics;
        os_get_window_metrics(w, &metrics);
        int width_t = metrics.width;
        int height_t = metrics.height;
        if (!metrics.visible) return;
        AudioLooperUi *self = static_cast<AudioLooperUi*>(w->parent_struct);
        int width, height;
        if (w->image) {
            os_get_surface_size(w->image, &width, &height);
            if ((width != width_t || height != height_t) || self->loadNew) {
                self->loadNew = false;
                self->create_waveview_image(w, width_t, height_t);
                os_get_surface_size(w->image, &width, &height);
            }
        } else {
            self->create_waveview_image(w, width_t, height_t);
            os_get_surface_size(w->image, &width, &height);
        }
        cairo_set_source_surface (w->crb, w->image, 0, 0);
        cairo_rectangle(w->crb,0, 0, width, height);
        cairo_fill(w->crb);

        double state = adj_get_state(w->adj);
        cairo_set_source_rgba(w->crb, 0.55, 0.05, 0.05, 1);
        cairo_rectangle(w->crb, 1+(width-2)*state,2,3, height-4);
        cairo_fill(w->crb);
    }

    static void boxShadowOutset(cairo_t* const cr, int x, int y, int width, int height, bool fill) {
        cairo_pattern_t *pat = cairo_pattern_create_linear (x, y, x + width, y);
        cairo_pattern_add_color_stop_rgba
            (pat, 0,0.33,0.33,0.33, 1.0);
        cairo_pattern_add_color_stop_rgba
            (pat, 0.1,0.33 * 0.6,0.33 * 0.6,0.33 * 0.6, 0.0);
        cairo_pattern_add_color_stop_rgba
            (pat, 0.97, 0.05 * 2.0, 0.05 * 2.0, 0.05 * 2.0, 0.0);
        cairo_pattern_add_color_stop_rgba
            (pat, 1, 0.05, 0.05, 0.05, 1.0);
        cairo_set_source(cr, pat);
        if (fill) cairo_fill_preserve (cr);
        else cairo_paint (cr);
        cairo_pattern_destroy (pat);
        pat = NULL;
        pat = cairo_pattern_create_linear (x, y, x, y + height);
        cairo_pattern_add_color_stop_rgba
            (pat, 0,0.33,0.33,0.33, 1.0);
        cairo_pattern_add_color_stop_rgba
            (pat, 0.1,0.33 * 0.6,0.33 * 0.6,0.33 * 0.6, 0.0);
        cairo_pattern_add_color_stop_rgba
            (pat, 0.97, 0.05 * 2.0, 0.05 * 2.0, 0.05 * 2.0, 0.0);
        cairo_pattern_add_color_stop_rgba
            (pat, 1, 0.05, 0.05, 0.05, 1.0);
        cairo_set_source(cr, pat);
        if (fill) cairo_fill_preserve (cr);
        else cairo_paint (cr);
        cairo_pattern_destroy (pat);
    }

    static void draw_window(void *w_, void* user_data) {
        Widget_t *w = (Widget_t*)w_;
        Metrics_t metrics;
        os_get_window_metrics(w, &metrics);
        int width = metrics.width;
        int height = metrics.height;
        if (!metrics.visible) return;

        cairo_pattern_t *pat;
        pat = cairo_pattern_create_linear (0.0, 0.0, width , height);
        cairo_pattern_add_color_stop_rgba (pat, 1, 0.2, 0.2, 0.2, 1);
        cairo_pattern_add_color_stop_rgba (pat, 0, 0, 0, 0., 1);
        cairo_rectangle(w->crb,0,0,width,height);
        cairo_set_source (w->crb, pat);
        cairo_fill_preserve (w->crb);
        boxShadowOutset(w->crb, 0.0, 0.0, width , height, true);
        cairo_fill (w->crb);
        cairo_pattern_destroy (pat);
    }

};

#endif

