#include "common/common.h"
#include <encoders/audio/audio.h>

obe_aud_enc_params_t *aud_enc_params_alloc()
{
        obe_aud_enc_params_t *aep = calloc(1, sizeof(*aep));
        aep->dialnorm = -31;
        aep->compression_gain = -1; /* Disabled */

        return aep;
}

void aud_enc_params_free(obe_aud_enc_params_t *aep)
{
	free(aep);
}
