/*
 * This is free and unencumbered software released into the public domain.

 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.

 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <gtkmm.h>
#include <fcntl.h>
#include <fstream>
#include <pthread.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <unistd.h> 

#include <sys/types.h>
#include <sys/stat.h>

#include <lilv/lilv.h>
#include "lv2/lv2plug.in/ns/ext/presets/presets.h"
#include "lv2/lv2plug.in/ns/ext/state/state.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"

#include "config.h"



///*** ----------- Class PresetList definition ----------- ***///

class PresetList {

    class Presets : public Gtk::TreeModel::ColumnRecord {
    public:
        Presets() {
            add(col_label);
            add(col_uri);
            add(col_plug);
        }
        ~Presets() {}
   
        Gtk::TreeModelColumn<Glib::ustring> col_label;
        Gtk::TreeModelColumn<Glib::ustring> col_uri;
        Gtk::TreeModelColumn<const LilvPlugin*> col_plug;
       
    };
    Presets psets;

    Glib::RefPtr<Gtk::ListStore> presetStore;
    Gtk::TreeModel::Row row ;
    
    int write_state_to_file(Glib::ustring state);
    void on_preset_selected(Gtk::Menu *presetMenu, Glib::ustring id, Gtk::TreeModel::iterator iter, LilvWorld* world);
    void on_preset_default(Gtk::Menu *presetMenu, Glib::ustring id);
    void create_preset_menu(Glib::ustring id, LilvWorld* world);
    void on_preset_key(GdkEventKey *ev,Gtk::Menu *presetMenu);

    static char** uris;
    static size_t n_uris;
    static const char* unmap_uri(LV2_URID_Map_Handle handle, LV2_URID urid);
    static LV2_URID map_uri(LV2_URID_Map_Handle handle, const char* uri);

    public:
    Glib::ustring interpret;
    Glib::RefPtr<Gtk::TreeView::Selection> selection;

    void create_preset_list(Glib::ustring id, const LilvPlugin* plug, LilvWorld* world);
    
    PresetList() {
        presetStore = Gtk::ListStore::create(psets);
        presetStore->set_sort_column(psets.col_label, Gtk::SORT_ASCENDING); 
    }

    ~PresetList() { 
        free(uris);
    }

};

///*** ----------- Class Options definition ----------- ***///

class Options : public Glib::OptionContext {
    Glib::OptionGroup o_group;
    Glib::OptionEntry opt_hide;
    Glib::OptionEntry opt_size;
public:
    bool hidden;
    int w_high;
    Options() :
        o_group("",""),
        hidden(false),
        w_high(0) {
        opt_hide.set_short_name('s');
        opt_hide.set_long_name("systray");
        opt_hide.set_description("start minimized in systray");

        opt_size.set_short_name('H');
        opt_size.set_long_name("high");
        opt_size.set_description("start with given high in pixel");
        opt_size.set_arg_description("HIGH");

        o_group.add_entry(opt_hide, hidden);
        o_group.add_entry(opt_size, w_high);
        set_main_group(o_group);
    }
    ~Options() {}
};


template <class T>
inline std::string to_string(const T& t) {
    std::stringstream ss;
    ss << t;
    return ss.str();
}


///*** ----------- Class KeyGrabber definition ----------- ***///

class LV2PluginList; // forward declaration (need not be resolved for KeyGrabber definition)

class KeyGrabber {
private:
    Display* dpy;
    XEvent ev;
    uint32_t modifiers;
    int32_t keycode;
    pthread_t m_pthr;
    void stop_keygrab_thread();
    void start_keygrab_thread();
    void keygrab();
    bool err;

    static void *run_keygrab_thread(void* p);
    static int my_XErrorHandler(Display * d, XErrorEvent * e);

    KeyGrabber()  {
        start_keygrab_thread();
    }
    ~KeyGrabber() {
        stop_keygrab_thread();
    }

public:
    LV2PluginList *runner;
    static KeyGrabber *get_instance();
};

///*** ----------- Class FiFoChannel definition ----------- ***///

class FiFoChannel {
private:
    int read_fd;
    Glib::RefPtr<Glib::IOChannel> iochannel;
    sigc::connection connect_io;
    static bool read_fifo(Glib::IOCondition io_condition);
    int open_fifo();
    void close_fifo();

    FiFoChannel() {
        open_fifo();
    }

    ~FiFoChannel() {
        close_fifo();
    }

public:
    bool is_mine;
    Glib::ustring own_pid;
    void write_fifo(Glib::IOCondition io_condition,Glib::ustring buf);
    LV2PluginList *runner;
    static FiFoChannel *get_instance();
};


///*** ----------- Class LV2PluginList definition ----------- ***///

class LV2PluginList : public Gtk::Window {

    class PlugInfo : public Gtk::TreeModel::ColumnRecord {
    public:
        PlugInfo() {
            add(col_id);
            add(col_name);
            add(col_tip);
            add(col_plug);
        }
        ~PlugInfo() {}
   
        Gtk::TreeModelColumn<Glib::ustring> col_id;
        Gtk::TreeModelColumn<Glib::ustring> col_name;
        Gtk::TreeModelColumn<Glib::ustring> col_tip;
        Gtk::TreeModelColumn<const LilvPlugin*> col_plug;
    };
    PlugInfo pinfo;

    std::vector<Glib::ustring> cats;
    Gtk::VBox topBox;
    Gtk::HBox buttonBox;
    Gtk::ComboBoxText comboBox;
    Gtk::ScrolledWindow scrollWindow;
    Gtk::Button buttonQuit;
    Gtk::Button newList;
    Gtk::ComboBoxText textEntry;
    Gtk::TreeView treeView;
    Gtk::TreeModel::Row row ;
    Gtk::Menu MenuPopup;
    Gtk::MenuItem menuQuit;
    int32_t mainwin_x;
    int32_t mainwin_y;
    int32_t valid_plugs;
    int32_t invalid_plugs;

    PresetList pstore;
    Glib::RefPtr<Gtk::StatusIcon> status_icon;
    Glib::RefPtr<Gtk::ListStore> listStore;
    Glib::RefPtr<Gtk::TreeView::Selection> selection;
    Glib::ustring regex;
    bool new_world;

    LilvWorld* world;
    const LilvPlugins* lv2_plugins;
    LV2_URID_Map map;
    LV2_Feature map_feature;
    KeyGrabber *kg;
    FiFoChannel *fc;
    
    void get_interpreter();
    void fill_list();
    void refill_list();
    void new_list();
    void fill_class_list();
    void systray_menu(guint button, guint32 activate_time);
    void show_preset_menu();
    void button_release_event(GdkEventButton *ev);
    bool key_release_event(GdkEventKey *ev);
    
    virtual void on_combo_changed();
    virtual void on_entry_changed();
    virtual void on_button_quit();

    public:
    Options options;
    void systray_hide();
    void come_up();
    void go_down();

    LV2PluginList() :
        buttonQuit("_Quit", true),
        newList("_Refresh", true),
        textEntry(true),
        mainwin_x(-1),
        mainwin_y(-1),
        valid_plugs(0),
        invalid_plugs(0),
        status_icon(Gtk::StatusIcon::create_from_file(PIXMAPS_DIR "/lv2.png")),
        new_world(false) {
        set_title("LV2 plugs");
        set_default_size(350,200);
        get_interpreter();
        fc = FiFoChannel::get_instance();
        fc->runner = this;
        fc->own_pid = "";
        kg = KeyGrabber::get_instance();
        kg->runner = this;

        listStore = Gtk::ListStore::create(pinfo);
        treeView.set_model(listStore);
        treeView.append_column("Name", pinfo.col_name);
        treeView.set_tooltip_column(2);
        treeView.set_rules_hint(true);
        listStore->set_sort_column(pinfo.col_name, Gtk::SORT_ASCENDING );
        fill_list();
        fill_class_list();

        scrollWindow.add(treeView);
        scrollWindow.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        scrollWindow.get_vscrollbar()->set_can_focus(true);
        topBox.pack_start(scrollWindow);
        topBox.pack_end(buttonBox,Gtk::PACK_SHRINK);
        buttonBox.pack_start(comboBox,Gtk::PACK_SHRINK);
        buttonBox.pack_start(textEntry,Gtk::PACK_EXPAND_WIDGET);
        buttonBox.pack_start(newList,Gtk::PACK_SHRINK);
        buttonBox.pack_start(buttonQuit,Gtk::PACK_SHRINK);
        add(topBox);

        set_icon_from_file(PIXMAPS_DIR "/lv2_16.png");
        menuQuit.set_label("Quit");
        MenuPopup.append(menuQuit);

        selection = treeView.get_selection();
        pstore.selection = selection;
        treeView.signal_button_release_event().connect_notify(sigc::mem_fun(*this, &LV2PluginList::button_release_event));
        treeView.signal_key_release_event().connect(sigc::mem_fun(*this, &LV2PluginList::key_release_event));
        buttonQuit.signal_clicked().connect( sigc::mem_fun(*this, &LV2PluginList::on_button_quit));
        newList.signal_clicked().connect( sigc::mem_fun(*this, &LV2PluginList::new_list));
        comboBox.signal_changed().connect( sigc::mem_fun(*this, &LV2PluginList::on_combo_changed));
        textEntry.signal_changed().connect( sigc::mem_fun(*this, &LV2PluginList::on_entry_changed));
        status_icon->signal_activate().connect( sigc::mem_fun(*this, &LV2PluginList::systray_hide));
        status_icon->signal_popup_menu().connect( sigc::mem_fun(*this, &LV2PluginList::systray_menu));
        menuQuit.signal_activate().connect( sigc::mem_fun(*this, &LV2PluginList::on_button_quit));
        show_all();
        treeView.grab_focus();
    }
    ~LV2PluginList() {
        lilv_world_free(world);
    }
};

///*** ----------- Class PresetList functions ----------- ***///

char** PresetList::uris = NULL;
size_t PresetList::n_uris = 0;

LV2_URID PresetList::map_uri(LV2_URID_Map_Handle handle, const char* uri) {
    for (size_t i = 0; i < n_uris; ++i) {
        if (!strcmp(uris[i], uri)) {
            return i + 1;
        }
    }

    uris = (char**)realloc(uris, ++n_uris * sizeof(char*));
    uris[n_uris - 1] = const_cast<char*>(uri);
    return n_uris;
}

const char* PresetList::unmap_uri(LV2_URID_Map_Handle handle, LV2_URID urid) {
    if (urid > 0 && urid <= n_uris) {
        return uris[urid - 1];
    }
    return NULL;
}

int PresetList::write_state_to_file(Glib::ustring state) {
    Glib::RefPtr<Gio::File> file = Gio::File::create_for_path("/tmp/state.ttl");
    if (file) {
        Glib::ustring prefix = "@prefix atom: <http://lv2plug.in/ns/ext/atom#> .\n"
                               "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
                               "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
                               "@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n"
                               "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                               "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n"
                               "@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .\n";
        prefix +=   state;
        Glib::RefPtr<Gio::DataOutputStream> out = Gio::DataOutputStream::create(file->replace());
        out->put_string(prefix);
        return 1;
    }
    return 0;

}

void PresetList::on_preset_selected(Gtk::Menu *presetMenu, Glib::ustring id, Gtk::TreeModel::iterator iter, LilvWorld* world) {
    Gtk::TreeModel::Row row = *iter;
    LV2_URID_Map       map           = { NULL, map_uri };
    LV2_URID_Unmap     unmap         = { NULL, unmap_uri };

    LilvNode* preset = lilv_new_uri(world, row.get_value(psets.col_uri).c_str());

    LilvState* state = lilv_state_new_from_world(world, &map, preset);
    lilv_state_set_label(state, row.get_value(psets.col_label).c_str());
    Glib::ustring st = lilv_state_to_string(world,&map,&unmap,state,row.get_value(psets.col_uri).c_str(),NULL);
    lilv_node_free(preset);
    lilv_state_free(state);
    st.replace(st.find_first_of("<"),st.find_first_of(">"),"<");
    if(!write_state_to_file(st)) return;
   
    Gtk::TreeModel::iterator it = selection->get_selected();
    if(iter) selection->unselect(*it);
    //Glib::ustring pre = row.get_value(psets.col_uri);
    //Glib::ustring com = interpret + " -p " + pre + id;
    Glib::ustring com = interpret + " -l " + "/tmp/state.ttl" + id;
    if (system(NULL)) system( com.c_str());
    presetStore->clear();
    delete presetMenu;
}

void PresetList::on_preset_default(Gtk::Menu *presetMenu, Glib::ustring id) {
    Gtk::TreeModel::iterator iter = selection->get_selected();
    if(iter) selection->unselect(*iter);
    Glib::ustring com = interpret + id;
    if (system(NULL)) system( com.c_str());
    presetStore->clear();
    delete presetMenu;
}

void PresetList::on_preset_key(GdkEventKey *ev,Gtk::Menu *presetMenu) {
    if (ev->keyval == 0xff1b) { // GDK_KEY_Escape
        presetStore->clear();
        delete presetMenu;
    }
}

void PresetList::create_preset_menu(Glib::ustring id, LilvWorld* world) {
    Gtk::MenuItem* item;
    Gtk::Menu *presetMenu = Gtk::manage(new Gtk::Menu());
    presetMenu->signal_key_release_event().connect_notify(
          sigc::bind(sigc::mem_fun(
          *this, &PresetList::on_preset_key),presetMenu));
    item = Gtk::manage(new Gtk::MenuItem("Default", true));
    item->signal_activate().connect_notify(
          sigc::bind(sigc::bind(sigc::mem_fun(
          *this, &PresetList::on_preset_default),id),presetMenu));
    presetMenu->append(*item);
    if (presetStore->children().end() != presetStore->children().begin()) {
        Gtk::SeparatorMenuItem *hline = Gtk::manage(new Gtk::SeparatorMenuItem());
        presetMenu->append(*hline);
        for (Gtk::TreeModel::iterator i = presetStore->children().begin();
                                  i != presetStore->children().end(); i++) {
            Gtk::TreeModel::Row row = *i; 
            item = Gtk::manage(new Gtk::MenuItem(row[psets.col_label], true));
            item->signal_activate().connect(
              sigc::bind(sigc::bind(sigc::bind(sigc::bind(sigc::mem_fun(
              *this, &PresetList::on_preset_selected),world),i),id),presetMenu));
            presetMenu->append(*item);
            
        }
    }
    presetMenu->show_all();
    presetMenu->popup(0,gtk_get_current_event_time());
}

void PresetList::create_preset_list(Glib::ustring id, const LilvPlugin* plug, LilvWorld* world) {
    LilvNodes* presets = lilv_plugin_get_related(plug,
      lilv_new_uri(world,LV2_PRESETS__Preset));
    presetStore->clear();
    LILV_FOREACH(nodes, i, presets) {
        const LilvNode* preset = lilv_nodes_get(presets, i);
        lilv_world_load_resource(world, preset);
        LilvNodes* labels = lilv_world_find_nodes(
          world, preset, lilv_new_uri(world, LILV_NS_RDFS "label"), NULL);
        if (labels) {
            const LilvNode* label = lilv_nodes_get_first(labels);
            Glib::ustring set =  lilv_node_as_string(label);
            row = *(presetStore->append());
            row[psets.col_label]=set;
            row[psets.col_uri] = lilv_node_as_uri(preset);
            row[psets.col_plug] = plug;
            lilv_nodes_free(labels);
        } else {
            fprintf(stderr, "Preset <%s> has no rdfs:label\n",
                    lilv_node_as_string(lilv_nodes_get(presets, i)));
        }
    }
    lilv_nodes_free(presets);
    create_preset_menu(id, world);
}



///*** ----------- Class KeyGrabber functions ----------- ***///

KeyGrabber*  KeyGrabber::get_instance() {
    static KeyGrabber instance;
    return &instance;
}

int KeyGrabber::my_XErrorHandler(Display * d, XErrorEvent * e)
{
    static int count = 0;
    KeyGrabber *kg = KeyGrabber::get_instance();
    if (!count) {
        fprintf(stderr, "X Error: try ControlMask | ShiftMask now \n");
        XUngrabKey(kg->dpy, kg->keycode, kg->modifiers, DefaultRootWindow(kg->dpy));
        kg->modifiers =  ControlMask | ShiftMask;
        XGrabKey(kg->dpy, kg->keycode, kg->modifiers, DefaultRootWindow(kg->dpy), 0, GrabModeAsync, GrabModeAsync);
        count +=1;
    } else {
        char buffer1[1024];
        XGetErrorText(d, e->error_code, buffer1, 1024);
        fprintf(stderr, "X Error:  %s\n Global HotKey disabled\n", buffer1);
        XUngrabKey(kg->dpy, kg->keycode, kg->modifiers, DefaultRootWindow(kg->dpy));
        XFlush(kg->dpy);
        XCloseDisplay(kg->dpy);
        kg->stop_keygrab_thread();
    }
    return 0;
}

void KeyGrabber::keygrab() {
    dpy = XOpenDisplay(0);
    XSetErrorHandler(my_XErrorHandler);
    modifiers =  ShiftMask;
    keycode = XKeysymToKeycode(dpy,XK_Escape);
    XGrabKey(dpy, keycode, modifiers, DefaultRootWindow(dpy), 0, GrabModeAsync, GrabModeAsync);
    XSync(dpy,true);
    XSelectInput(dpy,DefaultRootWindow(dpy), KeyPressMask);
    while(1) {
        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress)
            Glib::signal_idle().connect_once(
              sigc::mem_fun(static_cast<LV2PluginList *>(runner), 
              &LV2PluginList::systray_hide));
    }
}

void *KeyGrabber::run_keygrab_thread(void *p) {
    KeyGrabber *kg = KeyGrabber::get_instance();
    kg->keygrab();
    return NULL;
}

void KeyGrabber::stop_keygrab_thread() {
    pthread_cancel(m_pthr);
    pthread_join(m_pthr, NULL);
}

void KeyGrabber::start_keygrab_thread() {
    pthread_attr_t      attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE );
    pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    if (pthread_create(&m_pthr, &attr, run_keygrab_thread, NULL)) {
        err = true;
    }
    pthread_attr_destroy(&attr);
}

///*** ----------- Class LV2PluginList functions ----------- ***///

void LV2PluginList::get_interpreter() {
    if (system(NULL) )
      system("echo $PATH | tr ':' '\n' | xargs ls  | grep jalv | gawk '{if ($0 == \"jalv\") {print \"jalv -s\"} else {print $0}}' >/tmp/jalv.interpreter" );
    std::ifstream input( "/tmp/jalv.interpreter" );
    int s = 0;
    for( std::string line; getline( input, line ); ) {
        if ((line.compare("jalv.select") !=0)){
            comboBox.append(line);
            if ((line.compare("jalv.gtk") ==0))
                comboBox.set_active(s);
                s++;
        }
    }        
    pstore.interpret = comboBox.get_active_text(); 
}

void LV2PluginList::fill_list() {
    valid_plugs = 0;
    invalid_plugs = 0;
    Glib::ustring tool_tip;
    Glib::ustring tip;
    Glib::ustring tipby = " \nby ";
    world = lilv_world_new();
    lilv_world_load_all(world);
    lv2_plugins = lilv_world_get_all_plugins(world);        
    LilvNode* nd = NULL;
    LilvIter* it = lilv_plugins_begin(lv2_plugins);

    for (it; !lilv_plugins_is_end(lv2_plugins, it);
    it = lilv_plugins_next(lv2_plugins, it)) {
        const LilvPlugin* plug = lilv_plugins_get(lv2_plugins, it);
        if (plug) {
            nd = lilv_plugin_get_name(plug);
        }
        if (nd) {
            row = *(listStore->append());
            row[pinfo.col_name] = lilv_node_as_string(nd);
            row[pinfo.col_plug] = plug;
            row[pinfo.col_id] = lilv_node_as_string(lilv_plugin_get_uri(plug));
            valid_plugs++;
        } else {
            invalid_plugs++;
            continue;
        }
        lilv_node_free(nd);
        const LilvPluginClass* cls = lilv_plugin_get_class(plug);
        tip = lilv_node_as_string(lilv_plugin_class_get_label(cls));
        cats.insert(cats.begin(),tip);
        nd = lilv_plugin_get_author_name(plug);
        if (!nd) {
            nd = lilv_plugin_get_project(plug);
        }
        if (nd) {
             tip += tipby + lilv_node_as_string(nd);
        }
        lilv_node_free(nd);
        row[pinfo.col_tip] = tip;
    }
    tool_tip = to_string(valid_plugs)+" valid plugins installed\n";
    tool_tip += to_string(invalid_plugs)+" invalid plugins found";
    status_icon->set_tooltip_text(tool_tip);
}

void LV2PluginList::refill_list() {
    Glib::ustring name;
    Glib::ustring name_search;
    Glib::ustring tip;
    Glib::ustring tipby = " \nby ";
    LilvNode* nd;
    LilvIter* it = lilv_plugins_begin(lv2_plugins);
    for (it; !lilv_plugins_is_end(lv2_plugins, it);
    it = lilv_plugins_next(lv2_plugins, it)) {
        const LilvPlugin* plug = lilv_plugins_get(lv2_plugins, it);
        if (plug) {
            nd = lilv_plugin_get_name(plug);
        }
        if (nd) {
            name = lilv_node_as_string(nd);
            const LilvPluginClass* cls = lilv_plugin_get_class(plug);
            tip = lilv_node_as_string(lilv_plugin_class_get_label(cls));
            name_search = name+tip;
        } else {
            continue;
        }
        lilv_node_free(nd);
        Glib::ustring::size_type found = name_search.lowercase().find(regex.lowercase());
        if (found!=Glib::ustring::npos){
            row = *(listStore->append());
            row[pinfo.col_plug] = plug;
            row[pinfo.col_id] = lilv_node_as_string(lilv_plugin_get_uri(plug));
            row[pinfo.col_name] = name;
            nd = lilv_plugin_get_author_name(plug);
            if (!nd) {
                nd = lilv_plugin_get_project(plug);
            }
            if (nd) {
                tip += tipby + lilv_node_as_string(nd);
            }
            lilv_node_free(nd);
            row[pinfo.col_tip] = tip;
        }
    }
}

void LV2PluginList::new_list() {
    new_world = true;
    listStore->clear();
    lilv_world_free(world);
    world = NULL;
    textEntry.get_entry()->set_text("");
    fill_list();
}

void LV2PluginList::fill_class_list() {
    sort(cats.begin(), cats.end());
    cats.erase( unique(cats.begin(), cats.end()), cats.end());
    for (std::vector<Glib::ustring>::iterator it = cats.begin() ; it != cats.end(); ++it)
        textEntry.append_text(*it);
}

void LV2PluginList::on_entry_changed() {
    if(! new_world) {
        regex = textEntry.get_entry()->get_text();
        listStore->clear();
        refill_list();
    } else {
        new_world = false;
    }
}

void LV2PluginList::on_combo_changed() {
    pstore.interpret = comboBox.get_active_text();
}

void LV2PluginList::show_preset_menu() {
    Gtk::TreeModel::iterator iter = selection->get_selected();
    if(iter) {  
        Gtk::TreeModel::Row row = *iter;
        const LilvPlugin* plug = row[pinfo.col_plug];
        Glib::ustring id = " " + row[pinfo.col_id] + " & " ;
        pstore.create_preset_list( id, plug, world);
    }
}

void LV2PluginList::button_release_event(GdkEventButton *ev) {
    if (ev->type == GDK_BUTTON_RELEASE && ev->button == 1) {
        show_preset_menu();
    }
}

bool LV2PluginList::key_release_event(GdkEventKey *ev) {
    if (ev->keyval == 0xff0d || ev->keyval == 0x020 ) { // GDK_KEY_Return || GDK_KEY_space
        show_preset_menu();
    } else if ((ev->state & GDK_CONTROL_MASK) &&
           (ev->keyval ==  0x071) || (ev->keyval  ==  0x051)) { // GDK_KEY_q || GDK_KEY_Q
        on_button_quit();
    } else if ((ev->state & GDK_CONTROL_MASK) &&
           (ev->keyval ==  0x072) || (ev->keyval ==  0x052)) { // GDK_KEY_r || GDK_KEY_R 
        new_list();
    } else if ((ev->state & GDK_CONTROL_MASK) &&
           (ev->keyval ==  0x077) || (ev->keyval ==  0x057)) { // GDK_KEY_w || GDK_KEY_W 
        systray_hide();
    }
    return true;
}

void LV2PluginList::systray_menu(guint button, guint32 activate_time) {
    MenuPopup.show_all();
    MenuPopup.popup(2, gtk_get_current_event_time());
}

void LV2PluginList::systray_hide() {
    if (get_window()->get_state()
     & (Gdk::WINDOW_STATE_ICONIFIED|Gdk::WINDOW_STATE_WITHDRAWN)) {
        if(!options.hidden) {
            move(mainwin_x, mainwin_y);
        } else {
            options.hidden = false;
        }
        present();
    } else {
        get_window()->get_root_origin(mainwin_x, mainwin_y);
        hide();
    }
}

void LV2PluginList::come_up() {
    if (get_window()->get_state()
     & (Gdk::WINDOW_STATE_ICONIFIED|Gdk::WINDOW_STATE_WITHDRAWN)) {
        if(!options.hidden)
            move(mainwin_x, mainwin_y);
    } else {
        get_window()->get_root_origin(mainwin_x, mainwin_y);
    }
    present();
}

void LV2PluginList::go_down() {
    if (get_window()->get_state()
     & (Gdk::WINDOW_STATE_ICONIFIED|Gdk::WINDOW_STATE_WITHDRAWN)) {
        return;
    } else {
        get_window()->get_root_origin(mainwin_x, mainwin_y);
    }
    hide();
}

void LV2PluginList::on_button_quit() {
    Gtk::Main::quit();
}

///*** ----------- Class FiFoChannel functions ----------- ***///

FiFoChannel*  FiFoChannel::get_instance() {
    static FiFoChannel instance;
    return &instance;
}

bool FiFoChannel::read_fifo(Glib::IOCondition io_condition)
{
    FiFoChannel *fc = FiFoChannel::get_instance();
    if ((io_condition & Glib::IO_IN) == 0) {
        return false;
    } else {
        Glib::ustring buf;
        fc->iochannel->read_line(buf);
        if (buf.compare("quit\n") == 0){
            Gtk::Main::quit ();
        } else if (buf.compare("exit\n") == 0) {
            exit(0);
        } else if (buf.compare("show\n") == 0) {
            fc->runner->come_up();
        } else if (buf.compare("hide\n") == 0) {
            fc->runner->go_down();
        } else if (buf.find("PID: ") != Glib::ustring::npos) {
            fc->own_pid +="\n";
            if(buf.compare(fc->own_pid) != 0)
                fc->write_fifo(Glib::IO_OUT,"exit");
            fc->runner->come_up();
        } else {
            fprintf(stderr,"jalv.select * Unknown Message\n") ;
        }
    }
    return true;
}

void FiFoChannel::write_fifo(Glib::IOCondition io_condition, Glib::ustring buf)
{
    if ((io_condition & Glib::IO_OUT) == 0) {
        return;
    } else {
        connect_io.disconnect();
        iochannel->write(buf);
        iochannel->write("\n");
        iochannel->flush();
        connect_io = Glib::signal_io().connect(
          sigc::ptr_fun(read_fifo), read_fd, Glib::IO_IN);
    }
}

int FiFoChannel::open_fifo() {
    is_mine = false;
    if (access("/tmp/jalv.select.fifo", F_OK) == -1) {
        is_mine = true;
        if (mkfifo("/tmp/jalv.select.fifo", 0666) != 0) return -1;
    }
    read_fd = open("/tmp/jalv.select.fifo", O_RDWR | O_NONBLOCK);
    if (read_fd == -1) return -1;
    connect_io = Glib::signal_io().connect(
      sigc::ptr_fun(read_fifo), read_fd, Glib::IO_IN);
    iochannel = Glib::IOChannel::create_from_fd(read_fd);
}

void FiFoChannel::close_fifo() {
    if (is_mine) unlink("/tmp/jalv.select.fifo");
}

///*** ----------- main ----------- ***///

int main (int argc , char ** argv) {
    Gtk::Main kit (argc, argv);
    LV2PluginList lv2plugs;

    FiFoChannel *fc = FiFoChannel::get_instance();
        fc->own_pid = "PID: ";
        fc->own_pid += to_string(getpid());

    try {
        lv2plugs.options.parse(argc, argv);
    } catch (Glib::OptionError& error) {
        fprintf(stderr,"%s\n",error.what().c_str()) ;
    }
    
    if(lv2plugs.options.hidden) lv2plugs.hide();
    if(lv2plugs.options.w_high) lv2plugs.resize(1, lv2plugs.options.w_high);

    if (!fc->is_mine) {
        fc->write_fifo(Glib::IO_OUT,fc->own_pid);
    }

    Gtk::Main::run();
    return 0;
}
