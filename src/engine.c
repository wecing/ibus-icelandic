/* vim:set et sts=4: */

#include <enchant.h>
#include "engine.h"

typedef struct _IBusEnchantEngine IBusEnchantEngine;
typedef struct _IBusEnchantEngineClass IBusEnchantEngineClass;

struct _IBusEnchantEngine {
     IBusEngine parent;

     /* members */
     GString *preedit;
     gint cursor_pos;

     IBusLookupTable *table;
};

struct _IBusEnchantEngineClass {
     IBusEngineClass parent;
};

/* functions prototype */
static void ibus_enchant_engine_class_init  (IBusEnchantEngineClass *klass);
static void ibus_enchant_engine_init        (IBusEnchantEngine      *engine);
static void ibus_enchant_engine_destroy     (IBusEnchantEngine      *engine);
static gboolean 
ibus_enchant_engine_process_key_event
(IBusEngine             *engine,
 guint                   keyval,
 guint                   keycode,
 guint                   modifiers);
static void ibus_enchant_engine_focus_in    (IBusEngine             *engine);
static void ibus_enchant_engine_focus_out   (IBusEngine             *engine);
static void ibus_enchant_engine_reset       (IBusEngine             *engine);
static void ibus_enchant_engine_enable      (IBusEngine             *engine);
static void ibus_enchant_engine_disable     (IBusEngine             *engine);
static void ibus_engine_set_cursor_location (IBusEngine             *engine,
                                             gint                    x,
                                             gint                    y,
                                             gint                    w,
                                             gint                    h);
static void ibus_enchant_engine_set_capabilities
(IBusEngine             *engine,
 guint                   caps);
static void ibus_enchant_engine_page_up     (IBusEngine             *engine);
static void ibus_enchant_engine_page_down   (IBusEngine             *engine);
static void ibus_enchant_engine_cursor_up   (IBusEngine             *engine);
static void ibus_enchant_engine_cursor_down (IBusEngine             *engine);
static void ibus_enchant_property_activate  (IBusEngine             *engine,
                                             const gchar            *prop_name,
                                             gint                    prop_state);
static void ibus_enchant_engine_property_show
(IBusEngine             *engine,
 const gchar            *prop_name);
static void ibus_enchant_engine_property_hide
(IBusEngine             *engine,
 const gchar            *prop_name);

static void ibus_enchant_engine_commit_string
(IBusEnchantEngine      *enchant,
 const gchar            *string);
static void ibus_enchant_engine_update      (IBusEnchantEngine      *enchant);

static EnchantBroker *broker = NULL;
static EnchantDict *dict = NULL;

G_DEFINE_TYPE (IBusEnchantEngine, ibus_enchant_engine, IBUS_TYPE_ENGINE)

static void
ibus_enchant_engine_class_init (IBusEnchantEngineClass *klass)
{
     IBusObjectClass *ibus_object_class = IBUS_OBJECT_CLASS (klass);
     IBusEngineClass *engine_class = IBUS_ENGINE_CLASS (klass);
    
     ibus_object_class->destroy = (IBusObjectDestroyFunc) ibus_enchant_engine_destroy;

     engine_class->process_key_event = ibus_enchant_engine_process_key_event;
}

static void
ibus_enchant_engine_init (IBusEnchantEngine *enchant)
{
     if (broker == NULL) {
          broker = enchant_broker_init ();
          dict = enchant_broker_request_dict (broker, "en");
     }

     enchant->preedit = g_string_new ("");
     enchant->cursor_pos = 0;

     enchant->table = ibus_lookup_table_new (9, 0, TRUE, TRUE);
     g_object_ref_sink (enchant->table);
}

static void
ibus_enchant_engine_destroy (IBusEnchantEngine *enchant)
{
     if (enchant->preedit) {
          g_string_free (enchant->preedit, TRUE);
          enchant->preedit = NULL;
     }

     if (enchant->table) {
          g_object_unref (enchant->table);
          enchant->table = NULL;
     }

     ((IBusObjectClass *) ibus_enchant_engine_parent_class)->destroy ((IBusObject *)enchant);
}

static void
ibus_enchant_engine_update_lookup_table (IBusEnchantEngine *enchant)
{
     gchar ** sugs;
     gint n_sug, i;
     gboolean retval;

     if (enchant->preedit->len == 0) {
          ibus_engine_hide_lookup_table ((IBusEngine *) enchant);
          return;
     }

     ibus_lookup_table_clear (enchant->table);
    
     sugs = enchant_dict_suggest (dict,
                                  enchant->preedit->str,
                                  enchant->preedit->len,
                                  &n_sug);

     if (sugs == NULL || n_sug == 0) {
          ibus_engine_hide_lookup_table ((IBusEngine *) enchant);
          return;
     }

     for (i = 0; i < n_sug; i++) {
          ibus_lookup_table_append_candidate (enchant->table, ibus_text_new_from_string (sugs[i]));
     }

     ibus_engine_update_lookup_table ((IBusEngine *) enchant, enchant->table, TRUE);

     if (sugs)
          enchant_dict_free_suggestions (dict, sugs);
}

static void
ibus_enchant_engine_update_preedit (IBusEnchantEngine *enchant)
{
     IBusText *text;
     gint retval;

     text = ibus_text_new_from_static_string (enchant->preedit->str);
     text->attrs = ibus_attr_list_new ();
    
     ibus_attr_list_append (text->attrs,
                            ibus_attr_underline_new (IBUS_ATTR_UNDERLINE_SINGLE, 0, enchant->preedit->len));

     if (enchant->preedit->len > 0) {
          retval = enchant_dict_check (dict, enchant->preedit->str, enchant->preedit->len);
          if (retval != 0) {
               ibus_attr_list_append (text->attrs,
                                      ibus_attr_foreground_new (0xff0000, 0, enchant->preedit->len));
          }
     }
    
     ibus_engine_update_preedit_text ((IBusEngine *)enchant,
                                      text,
                                      enchant->cursor_pos,
                                      TRUE);

}

/* commit preedit to client and update preedit */
static gboolean
ibus_enchant_engine_commit_preedit (IBusEnchantEngine *enchant)
{
     if (enchant->preedit->len == 0)
          return FALSE;
    
     ibus_enchant_engine_commit_string (enchant, enchant->preedit->str);
     g_string_assign (enchant->preedit, "");
     enchant->cursor_pos = 0;

     ibus_enchant_engine_update (enchant);

     return TRUE;
}


static void
ibus_enchant_engine_commit_string (IBusEnchantEngine *enchant,
                                   const gchar       *string)
{
     IBusText *text;
     text = ibus_text_new_from_static_string (string);
     ibus_engine_commit_text ((IBusEngine *)enchant, text);
}

static void
ibus_enchant_engine_update (IBusEnchantEngine *enchant)
{
     ibus_enchant_engine_update_preedit (enchant);
     ibus_engine_hide_lookup_table ((IBusEngine *)enchant);
}

#define is_alpha(c) (((c) >= IBUS_a && (c) <= IBUS_z) || ((c) >= IBUS_A && (c) <= IBUS_Z))

static gboolean 
ibus_enchant_engine_process_key_event (IBusEngine *engine,
                                       guint       keyval,
                                       guint       keycode,
                                       guint       modifiers)
{
     IBusText *text;
     IBusEnchantEngine *enchant = (IBusEnchantEngine *)engine;

     static gboolean _mkey_enabled = FALSE;
     static gboolean _engl_input = FALSE;

     if (modifiers & IBUS_RELEASE_MASK)
          return FALSE;

     modifiers &= (IBUS_CONTROL_MASK | IBUS_MOD1_MASK);

     if (modifiers == IBUS_CONTROL_MASK && keyval == IBUS_s) {
          ibus_enchant_engine_update_lookup_table (enchant);
          return TRUE;
     }

     if (modifiers != 0) {
          if (enchant->preedit->len == 0)
               return FALSE;
          else
               return TRUE;
     }


     switch (keyval) {
     case IBUS_space:
     case IBUS_Return:
     case IBUS_Escape:
          _mkey_enabled = FALSE;
          return FALSE;
     case IBUS_Shift_L:
     case IBUS_Shift_R:
          _engl_input = !_engl_input;
          _mkey_enabled = FALSE;
          return TRUE;
     }

     // a b c e d f g h i j k l m n o p <MODIFY-KEY> q  r s t u v w x y z
     if (is_alpha (keyval)) {
          gboolean _is_capital = TRUE;
          if('a' <= keyval && keyval <= 'z')
               _is_capital = FALSE;

          if(_engl_input) {
               g_string_insert_unichar(enchant->preedit, enchant->cursor_pos, keyval);
          } else {
          
               if(keyval == 'q' || keyval == 'Q') {
                    _mkey_enabled = !_mkey_enabled;
                    return TRUE;
               }

               // a b æ d e f g h i j k l m n o p <M> r s t u v ö x y ð á é í ó ú ý þ
               int _l_map[] = {97, 98, 230, 100, 101, 102, 103, 104, 105, 106, 107, 108,
                               109, 110, 111, 112, 95, 114, 115, 116, 117, 118, 246, 120,
                               121, 240, 225, 233, 237, 243, 250, 253, 254};
               // A B Æ D E F G H I J K L M N O P <M> R S T U V Ö X Y Ð Á É Í Ó Ú Ý Þ
               int _u_map[] = {65, 66, 198, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78,
                               79, 80, 95, 82, 83, 84, 85, 86, 214, 88, 89, 208, 193, 201,
                               205, 211, 218, 221, 222};

               if(_mkey_enabled) {
                    _mkey_enabled = FALSE;
                    char _mkey_list[] = "aeiouyt";
                    int i;
                    for(i = 0; i < 7; i++) {
                         if(keyval == _mkey_list[i] || keyval == _mkey_list[i] - 'a' + 'A') {
                              keyval = 26 + i + (_is_capital ? 'A' : 'a');
                         }
                    }
               }

               if(_is_capital) {
                    g_string_insert_unichar(enchant->preedit, enchant->cursor_pos, _u_map[keyval - 'A']);
               } else {
                    g_string_insert_unichar(enchant->preedit, enchant->cursor_pos, _l_map[keyval - 'a']);
               }

          }

          enchant->cursor_pos++;
          ibus_enchant_engine_update (enchant);

          return ibus_enchant_engine_commit_preedit (enchant);
        
          // return TRUE;
     }

     return FALSE;
}
