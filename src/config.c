/* config.c - vedi config.h.
 *
 * Fase 1: default impliciti e funzioni di supporto.
 * Il parsing di graph.cfg viene aggiunto in una fase successiva.
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>

void config_free(GraphConfig *cfg)
{
    if (!cfg)
        return;
    for (size_t i = 0; i < cfg->long_lived_count; i++)
        free(cfg->long_lived[i]);
    free(cfg->long_lived);
    for (size_t i = 0; i < cfg->override_count; i++) {
        free(cfg->overrides[i].branch);
        free(cfg->overrides[i].parent);
    }
    free(cfg->overrides);
    memset(cfg, 0, sizeof *cfg);
}

static bool name_in_list(char *const *names, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0)
            return true;
    }
    return false;
}

void config_set_implicit_trunk(GraphConfig *cfg, char *const *branch_names,
                               size_t branch_count)
{
    if (cfg->long_lived_count > 0)
        return; /* [long-lived] esplicito: nessun default */
    const char *trunk = NULL;
    if (name_in_list(branch_names, branch_count, "main"))
        trunk = "main";
    else if (name_in_list(branch_names, branch_count, "master"))
        trunk = "master";
    if (!trunk)
        return;
    cfg->long_lived = malloc(sizeof(char *));
    cfg->long_lived[0] = malloc(strlen(trunk) + 1);
    strcpy(cfg->long_lived[0], trunk);
    cfg->long_lived_count = 1;
}

int config_long_lived_rank(const GraphConfig *cfg, const char *branch_name)
{
    if (!cfg)
        return -1;
    for (size_t i = 0; i < cfg->long_lived_count; i++) {
        if (strcmp(cfg->long_lived[i], branch_name) == 0)
            return (int)i;
    }
    return -1;
}
