/*
 * Copyright (c) 2008-2009 Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "jaldi.h"

static inline u16 jaldi_hw_fbin2freq(u8 fbin, bool is2GHz)
{
	if (fbin == AR5416_BCHAN_UNUSED)
		return fbin;

	return (u16) ((is2GHz) ? (2300 + fbin) : (4800 + 5 * fbin));
}

void jaldi_hw_analog_shift_regwrite(struct jaldi_hw *hw, u32 reg, u32 val)
{
        REG_WRITE(hw, reg, val);

        if (hw->analog_shiftreg)
		udelay(100);
}

void jaldi_hw_analog_shift_rmw(struct jaldi_hw *hw, u32 reg, u32 mask,
			       u32 shift, u32 val)
{
	u32 regVal;

	regVal = REG_READ(hw, reg) & ~mask;
	regVal |= (val << shift) & mask;

	REG_WRITE(hw, reg, regVal);

	if (hw->analog_shiftreg)
		udelay(100);
}

int16_t jaldi_hw_interpolate(u16 target, u16 srcLeft, u16 srcRight,
			     int16_t targetLeft, int16_t targetRight)
{
	int16_t rv;

	if (srcRight == srcLeft) {
		rv = targetLeft;
	} else {
		rv = (int16_t) (((target - srcLeft) * targetRight +
				 (srcRight - target) * targetLeft) /
				(srcRight - srcLeft));
	}
	return rv;
}

bool jaldi_hw_get_lower_upper_index(u8 target, u8 *pList, u16 listSize,
				    u16 *indexL, u16 *indexR)
{
	u16 i;

	if (target <= pList[0]) {
		*indexL = *indexR = 0;
		return true;
	}
	if (target >= pList[listSize - 1]) {
		*indexL = *indexR = (u16) (listSize - 1);
		return true;
	}

	for (i = 0; i < listSize - 1; i++) {
		if (pList[i] == target) {
			*indexL = *indexR = i;
			return true;
		}
		if (target < pList[i + 1]) {
			*indexL = i;
			*indexR = (u16) (i + 1);
			return false;
		}
	}
	return false;
}

bool jaldi_hw_nvram_read(struct jaldi_hw *hw, u32 off, u16 *data)
{
	bool res = hw->bus_ops->eeprom_read(hw->sc, off, data);
//	jaldi_print(JALDI_DEBUG, "nvram_read mem: %lx offset: %8x, data: %4x\n", (unsigned long)hw->sc->hw->sc->mem, off, *data);
	return res;
}

void jaldi_hw_fill_vpd_table(u8 pwrMin, u8 pwrMax, u8 *pPwrList,
			     u8 *pVpdList, u16 numIntercepts,
			     u8 *pRetVpdList)
{
	u16 i, k;
	u8 currPwr = pwrMin;
	u16 idxL = 0, idxR = 0;

	for (i = 0; i <= (pwrMax - pwrMin) / 2; i++) {
		jaldi_hw_get_lower_upper_index(currPwr, pPwrList,
					       numIntercepts, &(idxL),
					       &(idxR));
		if (idxR < 1)
			idxR = 1;
		if (idxL == numIntercepts - 1)
			idxL = (u16) (numIntercepts - 2);
		if (pPwrList[idxL] == pPwrList[idxR])
			k = pVpdList[idxL];
		else
			k = (u16)(((currPwr - pPwrList[idxL]) * pVpdList[idxR] +
				   (pPwrList[idxR] - currPwr) * pVpdList[idxL]) /
				  (pPwrList[idxR] - pPwrList[idxL]));
		pRetVpdList[i] = (u8) k;
		currPwr += 2;
	}
}

void jaldi_hw_get_legacy_target_powers(struct jaldi_hw *hw,
				       struct jaldi_channel *chan,
				       struct cal_target_power_leg *powInfo,
				       u16 numChannels,
				       struct cal_target_power_leg *pNewPower,
				       u16 numRates, bool isExtTarget)
{
	struct chan_centers centers;
	u16 clo, chi;
	int i;
	int matchIndex = -1, lowIndex = -1;
	u16 freq;

	jaldi_hw_get_channel_centers(chan, &centers);
	freq = (isExtTarget) ? centers.ext_center : centers.ctl_center;

	if (freq <= jaldi_hw_fbin2freq(powInfo[0].bChannel,
				       IS_CHAN_2GHZ(chan))) {
		matchIndex = 0;
	} else {
		for (i = 0; (i < numChannels) &&
			     (powInfo[i].bChannel != AR5416_BCHAN_UNUSED); i++) {
			if (freq == jaldi_hw_fbin2freq(powInfo[i].bChannel,
						       IS_CHAN_2GHZ(chan))) {
				matchIndex = i;
				break;
			} else if (freq < jaldi_hw_fbin2freq(powInfo[i].bChannel,
						IS_CHAN_2GHZ(chan)) && i > 0 &&
				   freq > jaldi_hw_fbin2freq(powInfo[i - 1].bChannel,
						IS_CHAN_2GHZ(chan))) {
				lowIndex = i - 1;
				break;
			}
		}
		if ((matchIndex == -1) && (lowIndex == -1))
			matchIndex = i - 1;
	}

	if (matchIndex != -1) {
		*pNewPower = powInfo[matchIndex];
	} else {
		clo = jaldi_hw_fbin2freq(powInfo[lowIndex].bChannel,
					 IS_CHAN_2GHZ(chan));
		chi = jaldi_hw_fbin2freq(powInfo[lowIndex + 1].bChannel,
					 IS_CHAN_2GHZ(chan));

		for (i = 0; i < numRates; i++) {
			pNewPower->tPow2x[i] =
				(u8)jaldi_hw_interpolate(freq, clo, chi,
						powInfo[lowIndex].tPow2x[i],
						powInfo[lowIndex + 1].tPow2x[i]);
		}
	}
}

void jaldi_hw_get_target_powers(struct jaldi_hw *hw,
				struct jaldi_channel *chan,
				struct cal_target_power_ht *powInfo,
				u16 numChannels,
				struct cal_target_power_ht *pNewPower,
				u16 numRates, bool isHt40Target)
{
	struct chan_centers centers;
	u16 clo, chi;
	int i;
	int matchIndex = -1, lowIndex = -1;
	u16 freq;

	jaldi_hw_get_channel_centers(chan, &centers);
	freq = isHt40Target ? centers.synth_center : centers.ctl_center;

	if (freq <= jaldi_hw_fbin2freq(powInfo[0].bChannel, IS_CHAN_2GHZ(chan))) {
		matchIndex = 0;
	} else {
		for (i = 0; (i < numChannels) &&
			     (powInfo[i].bChannel != AR5416_BCHAN_UNUSED); i++) {
			if (freq == jaldi_hw_fbin2freq(powInfo[i].bChannel,
						       IS_CHAN_2GHZ(chan))) {
				matchIndex = i;
				break;
			} else
				if (freq < jaldi_hw_fbin2freq(powInfo[i].bChannel,
						IS_CHAN_2GHZ(chan)) && i > 0 &&
				    freq > jaldi_hw_fbin2freq(powInfo[i - 1].bChannel,
						IS_CHAN_2GHZ(chan))) {
					lowIndex = i - 1;
					break;
				}
		}
		if ((matchIndex == -1) && (lowIndex == -1))
			matchIndex = i - 1;
	}

	if (matchIndex != -1) {
		*pNewPower = powInfo[matchIndex];
	} else {
		clo = jaldi_hw_fbin2freq(powInfo[lowIndex].bChannel,
					 IS_CHAN_2GHZ(chan));
		chi = jaldi_hw_fbin2freq(powInfo[lowIndex + 1].bChannel,
					 IS_CHAN_2GHZ(chan));

		for (i = 0; i < numRates; i++) {
			pNewPower->tPow2x[i] = (u8)jaldi_hw_interpolate(freq,
						clo, chi,
						powInfo[lowIndex].tPow2x[i],
						powInfo[lowIndex + 1].tPow2x[i]);
		}
	}
}

u16 jaldi_hw_get_max_edge_power(u16 freq, struct cal_ctl_edges *pRdEdgesPower,
				bool is2GHz, int num_band_edges)
{
	u16 twiceMaxEdgePower = AR5416_MAX_RATE_POWER;
	int i;

	for (i = 0; (i < num_band_edges) &&
		     (pRdEdgesPower[i].bChannel != AR5416_BCHAN_UNUSED); i++) {
		if (freq == jaldi_hw_fbin2freq(pRdEdgesPower[i].bChannel, is2GHz)) {
			twiceMaxEdgePower = pRdEdgesPower[i].tPower;
			break;
		} else if ((i > 0) &&
			   (freq < jaldi_hw_fbin2freq(pRdEdgesPower[i].bChannel,
						      is2GHz))) {
			if (jaldi_hw_fbin2freq(pRdEdgesPower[i - 1].bChannel,
					       is2GHz) < freq &&
			    pRdEdgesPower[i - 1].flag) {
				twiceMaxEdgePower =
					pRdEdgesPower[i - 1].tPower;
			}
			break;
		}
	}

	return twiceMaxEdgePower;
}

int jaldi_hw_eeprom_init(struct jaldi_hw *hw)
{
	int status;

	if (AR_SREV_9300_20_OR_LATER(hw)) {
		jaldi_print(JALDI_DEBUG, "eeprom ar9300\n");
	//	hw->eep_ops = &eep_ar9300_ops;
	} else if (AR_SREV_9287(hw)) {
		jaldi_print(JALDI_DEBUG, "eeprom ar9287\n");
	//	hw->eep_ops = &eep_ar9287_ops;
	} else if (AR_SREV_9285(hw) || AR_SREV_9271(hw)) {
		jaldi_print(JALDI_DEBUG, "eeprom 4k\n");
	//	hw->eep_ops = &eep_4k_ops;
	} else {
		jaldi_print(JALDI_DEBUG, "eeprom def\n");
		hw->eep_ops = &eep_def_ops;
	}

	if (!hw->eep_ops->fill_eeprom(hw))
		return -EIO;

	status = hw->eep_ops->check_eeprom(hw);

	return status;
}
