/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2022-2023, Raspberry Pi Ltd
 *
 * af.cpp - Autofocus control algorithm
 */

#include "af.h"

#include <iomanip>
#include <math.h>
#include <stdlib.h>

#include <libcamera/base/log.h>

#include <libcamera/control_ids.h>
#include "../agc_status.h"

using namespace RPiController;
using namespace libcamera;

LOG_DEFINE_CATEGORY(RPiAf)

#define NAME "rpi.af"

/*
 * Default values for parameters. All may be overridden in the tuning file.
 * Many of these values are sensor- or module-dependent; the defaults here
 * assume IMX708 in a Raspberry Pi V3 camera with the standard lens.
 *
 * Here all focus values are in dioptres (1/m). They are converted to hardware
 * units when written to status.lensSetting or returned from setLensPosition().
 *
 * Gain and delay values are relative to the update rate, since much (not all)
 * of the delay is in the sensor and (for CDAF) ISP, not the lens mechanism;
 * but note that algorithms are updated at no more than 30 Hz.
 */

Af::RangeDependentParams::RangeDependentParams()
	: focusMin(0.0),
	  focusMax(12.0),
	  focusDefault(1.0)
{
}

Af::SpeedDependentParams::SpeedDependentParams()
	: stepCoarse(1.0),
	  stepFine(0.25),
	  contrastRatio(0.75),
	  pdafGain(-0.02),
	  pdafSquelch(0.125),
	  maxSlew(2.0),
	  pdafFrames(20),
	  dropoutFrames(6),
	  stepFrames(4)
{
}

Af::CfgParams::CfgParams()
	: confEpsilon(8),
	  confThresh(16),
	  confClip(512),
	  skipFrames(5),
	  map()
{
}

template<typename T>
static void readNumber(T &dest, const libcamera::YamlObject &params, char const *name)
{
	auto value = params[name].get<T>();
	if (value)
		dest = *value;
	else
		LOG(RPiAf, Warning) << "Missing parameter \"" << name << "\"";
}

void Af::RangeDependentParams::read(const libcamera::YamlObject &params)
{

	readNumber<double>(focusMin, params, "min");
	readNumber<double>(focusMax, params, "max");
	readNumber<double>(focusDefault, params, "default");
}

void Af::SpeedDependentParams::read(const libcamera::YamlObject &params)
{
	readNumber<double>(stepCoarse, params, "step_coarse");
	readNumber<double>(stepFine, params, "step_fine");
	readNumber<double>(contrastRatio, params, "contrast_ratio");
	readNumber<double>(pdafGain, params, "pdaf_gain");
	readNumber<double>(pdafSquelch, params, "pdaf_squelch");
	readNumber<double>(maxSlew, params, "max_slew");
	readNumber<uint32_t>(pdafFrames, params, "pdaf_frames");
	readNumber<uint32_t>(dropoutFrames, params, "dropout_frames");
	readNumber<uint32_t>(stepFrames, params, "step_frames");
}

int Af::CfgParams::read(const libcamera::YamlObject &params)
{
	if (params.contains("ranges")) {
		auto &rr = params["ranges"];

		if (rr.contains("normal"))
			ranges[AfRangeNormal].read(rr["normal"]);
		else
			LOG(RPiAf, Warning) << "Missing range \"normal\"";

		ranges[AfRangeMacro] = ranges[AfRangeNormal];
		if (rr.contains("macro"))
			ranges[AfRangeMacro].read(rr["macro"]);

		ranges[AfRangeFull].focusMin = std::min(ranges[AfRangeNormal].focusMin,
							ranges[AfRangeMacro].focusMin);
		ranges[AfRangeFull].focusMax = std::max(ranges[AfRangeNormal].focusMax,
							ranges[AfRangeMacro].focusMax);
		ranges[AfRangeFull].focusDefault = ranges[AfRangeNormal].focusDefault;
		if (rr.contains("full"))
			ranges[AfRangeFull].read(rr["full"]);
	} else
		LOG(RPiAf, Warning) << "No ranges defined";

	if (params.contains("speeds")) {
		auto &ss = params["speeds"];

		if (ss.contains("normal"))
			speeds[AfSpeedNormal].read(ss["normal"]);
		else
			LOG(RPiAf, Warning) << "Missing speed \"normal\"";

		speeds[AfSpeedFast] = speeds[AfSpeedNormal];
		if (ss.contains("fast"))
			speeds[AfSpeedFast].read(ss["fast"]);
	} else
		LOG(RPiAf, Warning) << "No speeds defined";

	readNumber<uint32_t>(confEpsilon, params, "conf_epsilon");
	readNumber<uint32_t>(confThresh, params, "conf_thresh");
	readNumber<uint32_t>(confClip, params, "conf_clip");
	readNumber<uint32_t>(skipFrames, params, "skip_frames");

	if (params.contains("map"))
		map.read(params["map"]);
	else
		LOG(RPiAf, Warning) << "No map defined";

	return 0;
}

void Af::CfgParams::initialise()
{
	if (map.empty()) {
		/* Default mapping from dioptres to hardware setting */
		static constexpr double DefaultMapX0 = 0.0;
		static constexpr double DefaultMapY0 = 445.0;
		static constexpr double DefaultMapX1 = 15.0;
		static constexpr double DefaultMapY1 = 925.0;

		map.append(DefaultMapX0, DefaultMapY0);
		map.append(DefaultMapX1, DefaultMapY1);
	}
}

/* Af Algorithm class */

static constexpr unsigned MaxWindows = 10;

Af::Af(Controller *controller)
	: AfAlgorithm(controller),
	  cfg_(),
	  range_(AfRangeNormal),
	  speed_(AfSpeedNormal),
	  mode_(AfAlgorithm::AfModeManual),
	  pauseFlag_(false),
	  statsRegion_(0, 0, 0, 0),
	  windows_(),
	  useWindows_(false),
	  phaseWeights_(),
	  contrastWeights_(),
	  scanState_(ScanState::Idle),
	  initted_(false),
	  ftarget_(-1.0),
	  fsmooth_(-1.0),
	  prevContrast_(0.0),
	  skipCount_(0),
	  stepCount_(0),
	  dropCount_(0),
	  scanMaxContrast_(0.0),
	  scanMinContrast_(1.0e9),
	  scanData_(),
	  reportState_(AfState::Idle)
{
	/*
	 * Reserve space for data, to reduce memory fragmentation. It's too early
	 * to query the size of the PDAF (from camera) and Contrast (from ISP)
	 * statistics, but these are plausible upper bounds.
	 */
	phaseWeights_.w.reserve(16 * 12);
	contrastWeights_.w.reserve(getHardwareConfig().focusRegions.width *
				   getHardwareConfig().focusRegions.height);
	scanData_.reserve(32);
	isPdafEnabled_ = false;
}

Af::~Af()
{
}

char const *Af::name() const
{
	return NAME;
}

int Af::read(const libcamera::YamlObject &params)
{
	return cfg_.read(params);
}

void Af::initialise()
{
	cfg_.initialise();
}

void Af::switchMode(CameraMode const &cameraMode, [[maybe_unused]] Metadata *metadata)
{
	(void)metadata;

	/* Assume that PDAF and Focus stats grids cover the visible area */
	statsRegion_.x = (int)cameraMode.cropX;
	statsRegion_.y = (int)cameraMode.cropY;
	statsRegion_.width = (unsigned)(cameraMode.width * cameraMode.scaleX);
	statsRegion_.height = (unsigned)(cameraMode.height * cameraMode.scaleY);
	LOG(RPiAf, Debug) << "switchMode: statsRegion: "
			  << statsRegion_.x << ','
			  << statsRegion_.y << ','
			  << statsRegion_.width << ','
			  << statsRegion_.height;
	invalidateWeights();

	if (scanState_ >= ScanState::Coarse && scanState_ < ScanState::Settle) {
		/*
		 * If a scan was in progress, re-start it, as CDAF statistics
		 * may have changed. Though if the application is just about
		 * to take a still picture, this will not help...
		 */
		startProgrammedScan();
	}
	skipCount_ = cfg_.skipFrames;
}

void Af::computeWeights(RegionWeights *wgts, unsigned rows, unsigned cols)
{
	wgts->rows = rows;
	wgts->cols = cols;
	wgts->sum = 0;
	wgts->w.resize(rows * cols);
	std::fill(wgts->w.begin(), wgts->w.end(), 0);

	if (rows > 0 && cols > 0 && useWindows_ &&
	    statsRegion_.height >= rows && statsRegion_.width >= cols) {
		/*
		 * Here we just merge all of the given windows, weighted by area.
		 * \todo Perhaps a better approach might be to find the phase in each
		 * window and choose either the closest or the highest-confidence one?
		 * Ensure weights sum to less than (1<<16). 46080 is a "round number"
		 * below 65536, for better rounding when window size is a simple
		 * fraction of image dimensions.
		 */
		const unsigned maxCellWeight = 46080u / (MaxWindows * rows * cols);
		const unsigned cellH = statsRegion_.height / rows;
		const unsigned cellW = statsRegion_.width / cols;
		const unsigned cellA = cellH * cellW;

		for (auto &w : windows_) {
			for (unsigned r = 0; r < rows; ++r) {
				int y0 = std::max(statsRegion_.y + (int)(cellH * r), w.y);
				int y1 = std::min(statsRegion_.y + (int)(cellH * (r + 1)),
						  w.y + (int)(w.height));
				if (y0 >= y1)
					continue;
				y1 -= y0;
				for (unsigned c = 0; c < cols; ++c) {
					int x0 = std::max(statsRegion_.x + (int)(cellW * c), w.x);
					int x1 = std::min(statsRegion_.x + (int)(cellW * (c + 1)),
							  w.x + (int)(w.width));
					if (x0 >= x1)
						continue;
					unsigned a = y1 * (x1 - x0);
					a = (maxCellWeight * a + cellA - 1) / cellA;
					wgts->w[r * cols + c] += a;
					wgts->sum += a;
				}
			}
		}
	}

	if (wgts->sum == 0) {
		/* Default AF window is the middle 1/2 width of the middle 1/3 height */
		for (unsigned r = rows / 3; r < rows - rows / 3; ++r) {
			for (unsigned c = cols / 4; c < cols - cols / 4; ++c) {
				wgts->w[r * cols + c] = 1;
				wgts->sum += 1;
			}
		}
	}
}

void Af::invalidateWeights()
{
	phaseWeights_.sum = 0;
	contrastWeights_.sum = 0;
}

bool Af::getPhase(PdafRegions const &regions, double &phase, double &conf)
{
	libcamera::Size size = regions.size();
	if (size.height != phaseWeights_.rows || size.width != phaseWeights_.cols ||
	    phaseWeights_.sum == 0) {
		LOG(RPiAf, Debug) << "Recompute Phase weights " << size.width << 'x' << size.height;
		computeWeights(&phaseWeights_, size.height, size.width);
	}

	uint32_t sumWc = 0;
	int64_t sumWcp = 0;
	for (unsigned i = 0; i < regions.numRegions(); ++i) {
		unsigned w = phaseWeights_.w[i];
		if (w) {
			const PdafData &data = regions.get(i).val;
			unsigned c = data.conf;
			if (c >= cfg_.confThresh) {
				if (c > cfg_.confClip)
					c = cfg_.confClip;
				c -= (cfg_.confThresh >> 2);
				sumWc += w * c;
				c -= (cfg_.confThresh >> 2);
				sumWcp += (int64_t)(w * c) * (int64_t)data.phase;
			}
		}
	}

	if (0 < phaseWeights_.sum && phaseWeights_.sum <= sumWc) {
		phase = (double)sumWcp / (double)sumWc;
		conf = (double)sumWc / (double)phaseWeights_.sum;
		return true;
	} else {
		phase = 0.0;
		conf = 0.0;
		return false;
	}
}

double Af::getContrast(const FocusRegions &focusStats)
{
	libcamera::Size size = focusStats.size();
	if (size.height != contrastWeights_.rows ||
	    size.width != contrastWeights_.cols || contrastWeights_.sum == 0) {
		LOG(RPiAf, Debug) << "Recompute Contrast weights "
				  << size.width << 'x' << size.height;
		computeWeights(&contrastWeights_, size.height, size.width);
	}

	uint64_t sumWc = 0;
	for (unsigned i = 0; i < focusStats.numRegions(); ++i)
		sumWc += contrastWeights_.w[i] * focusStats.get(i).val;

	return (contrastWeights_.sum > 0) ? ((double)sumWc / (double)contrastWeights_.sum) : 0.0;
}

void Af::doPDAF(double phase, double conf)
{
	/* Apply loop gain */
	phase *= cfg_.speeds[speed_].pdafGain;

	if (mode_ == AfModeContinuous) {
		/*
		 * PDAF in Continuous mode. Scale down lens movement when
		 * delta is small or confidence is low, to suppress wobble.
		 */
		phase *= conf / (conf + cfg_.confEpsilon);
		if (std::abs(phase) < cfg_.speeds[speed_].pdafSquelch) {
			double a = phase / cfg_.speeds[speed_].pdafSquelch;
			phase *= a * a;
		}
	} else {
		/*
		 * PDAF in triggered-auto mode. Allow early termination when
		 * phase delta is small; scale down lens movements towards
		 * the end of the sequence, to ensure a stable image.
		 */
		if (stepCount_ >= cfg_.speeds[speed_].stepFrames) {
			if (std::abs(phase) < cfg_.speeds[speed_].pdafSquelch)
				stepCount_ = cfg_.speeds[speed_].stepFrames;
		} else
			phase *= stepCount_ / cfg_.speeds[speed_].stepFrames;
	}

	/* Apply slew rate limit. Report failure if out of bounds. */
	if (phase < -cfg_.speeds[speed_].maxSlew) {
		phase = -cfg_.speeds[speed_].maxSlew;
		reportState_ = (ftarget_ <= cfg_.ranges[range_].focusMin) ? AfState::Failed
									  : AfState::Scanning;
	} else if (phase > cfg_.speeds[speed_].maxSlew) {
		phase = cfg_.speeds[speed_].maxSlew;
		reportState_ = (ftarget_ >= cfg_.ranges[range_].focusMax) ? AfState::Failed
									  : AfState::Scanning;
	} else
		reportState_ = AfState::Focused;

	ftarget_ = fsmooth_ + phase;
}

bool Af::earlyTerminationByPhase(double phase)
{
	if (scanData_.size() > 0 &&
	    scanData_[scanData_.size() - 1].conf >= cfg_.confEpsilon) {
		double oldFocus = scanData_[scanData_.size() - 1].focus;
		double oldPhase = scanData_[scanData_.size() - 1].phase;

		/*
		 * Check that the gradient is finite and has the expected sign;
		 * Interpolate/extrapolate the lens position for zero phase.
		 * Check that the extrapolation is well-conditioned.
		 */
		if ((ftarget_ - oldFocus) * (phase - oldPhase) > 0.0) {
			double param = phase / (phase - oldPhase);
			if (-3.0 <= param && param <= 3.5) {
				ftarget_ += param * (oldFocus - ftarget_);
				LOG(RPiAf, Debug) << "ETBP: param=" << param;
				return true;
			}
		}
	}

	return false;
}

double Af::findPeak(unsigned i) const
{
	double f = scanData_[i].focus;

	if (i > 0 && i + 1 < scanData_.size()) {
		double dropLo = scanData_[i].contrast - scanData_[i - 1].contrast;
		double dropHi = scanData_[i].contrast - scanData_[i + 1].contrast;
		if (0.0 <= dropLo && dropLo < dropHi) {
			double param = 0.3125 * (1.0 - dropLo / dropHi) * (1.6 - dropLo / dropHi);
			f += param * (scanData_[i - 1].focus - f);
		} else if (0.0 <= dropHi && dropHi < dropLo) {
			double param = 0.3125 * (1.0 - dropHi / dropLo) * (1.6 - dropHi / dropLo);
			f += param * (scanData_[i + 1].focus - f);
		}
	}

	LOG(RPiAf, Debug) << "FindPeak: " << f;
	return f;
}

void Af::doScan(double contrast, double phase, double conf)
{
	/* Record lens position, contrast and phase values for the current scan */
	if (scanData_.empty() || contrast > scanMaxContrast_) {
		scanMaxContrast_ = contrast;
		scanMaxIndex_ = scanData_.size();
	}
	if (contrast < scanMinContrast_)
		scanMinContrast_ = contrast;
	scanData_.emplace_back(ScanRecord{ ftarget_, contrast, phase, conf });

	if (scanState_ == ScanState::Coarse) {
		if (ftarget_ >= cfg_.ranges[range_].focusMax ||
		    contrast < cfg_.speeds[speed_].contrastRatio * scanMaxContrast_) {
			/*
			 * Finished course scan, or termination based on contrast.
			 * Jump to just after max contrast and start fine scan.
			 */
			ftarget_ = std::min(ftarget_, findPeak(scanMaxIndex_) +
					2.0 * cfg_.speeds[speed_].stepFine);
			scanState_ = ScanState::Fine;
			scanData_.clear();
		} else
			ftarget_ += cfg_.speeds[speed_].stepCoarse;
	} else { /* ScanState::Fine */
		if (ftarget_ <= cfg_.ranges[range_].focusMin || scanData_.size() >= 5 ||
		    contrast < cfg_.speeds[speed_].contrastRatio * scanMaxContrast_) {
			/*
			 * Finished fine scan, or termination based on contrast.
			 * Use quadratic peak-finding to find best contrast position.
			 */
			ftarget_ = findPeak(scanMaxIndex_);
			scanState_ = ScanState::Settle;
		} else
			ftarget_ -= cfg_.speeds[speed_].stepFine;
	}

	stepCount_ = (ftarget_ == fsmooth_) ? 0 : cfg_.speeds[speed_].stepFrames;
}

static void generate_stats(std::vector<double> &zones,
			   RgbyRegions stats, double min_pixels,
			   double min_G)
{
	for (const auto &iter : stats) {
		double zone;
		double counted = iter.counted;
		if (counted >= min_pixels) {
			zone = iter.val.gSum / counted;
			if (zone >= min_G) {
				zones.push_back(zone);
			}
		}
	}
}

void Af::doAF(double contrast, double phase, double conf)
{
	/* Skip frames at startup and after sensor mode change */
	if (skipCount_ > 0) {
		LOG(RPiAf, Debug) << "SKIP";
		skipCount_--;
		return;
	}

	if (mode_ == AfModeContinuous && !isPdafEnabled_ && scanState_ == ScanState::Idle) {
		AgcPrepareStatus agc_prepare_status;
		if (imageMetadata_->get("agc.prepare_status", agc_prepare_status) == 0) {
			LOG(RPiAf, Debug) << "AGC Locked: " << agc_prepare_status.locked;
		} else {
			agc_prepare_status.locked = 0;
		}
		std::vector<double> zones;
		zones.clear();
		generate_stats(zones, stats_->awbRegions, 16, 32);
		double sum = 0;
		double mean;
		for (double zone: zones) {
			sum += zone;
		}
		mean = sum / zones.size();
		if (agc_prepare_status.locked && last_mean != 0) {
			double mean_diff = std::abs(mean - last_mean) ;
			if (mean_diff > 1000) {
				trigger_when_stable = true;
				LOG(RPiAf, Debug) << "Diff > 1000.";
			}
			if (trigger_when_stable && mean_diff < 400)
				startProgrammedScan();
			else if (last_agc_status == 0 && agc_prepare_status.locked)
				startProgrammedScan();
		}
		last_agc_status = agc_prepare_status.locked;
		last_mean = mean;
	} else if (scanState_ == ScanState::Pdaf) {
		/*
		 * Use PDAF closed-loop control whenever available, in both CAF
		 * mode and (for a limited number of iterations) when triggered.
		 * If PDAF fails (due to poor contrast, noise or large defocus),
		 * fall back to a CDAF-based scan. To avoid "nuisance" scans,
		 * scan only after a number of frames with low PDAF confidence.
		 */
		if (conf > (dropCount_ ? 1.0 : 0.25) * cfg_.confEpsilon) {
			doPDAF(phase, conf);
			if (stepCount_ > 0)
				stepCount_--;
			else if (mode_ != AfModeContinuous)
				scanState_ = ScanState::Idle;
			dropCount_ = 0;
		} else if (++dropCount_ == cfg_.speeds[speed_].dropoutFrames)
			startProgrammedScan();
	} else if (scanState_ >= ScanState::Coarse && fsmooth_ == ftarget_) {
		/*
		 * Scanning sequence. This means PDAF has become unavailable.
		 * Allow a delay between steps for CDAF FoM statistics to be
		 * updated, and a "settling time" at the end of the sequence.
		 * [A coarse or fine scan can be abandoned if two PDAF samples
		 * allow direct interpolation of the zero-phase lens position.]
		 */
		if (stepCount_ > 0)
			stepCount_--;
		else if (scanState_ == ScanState::Settle) {
			if (prevContrast_ >= cfg_.speeds[speed_].contrastRatio * scanMaxContrast_ &&
			    scanMinContrast_ <= cfg_.speeds[speed_].contrastRatio * scanMaxContrast_)
				reportState_ = AfState::Focused;
			else
				reportState_ = AfState::Failed;
			if (mode_ == AfModeContinuous && !pauseFlag_ &&
			    cfg_.speeds[speed_].dropoutFrames > 0 && isPdafEnabled_)
				scanState_ = ScanState::Pdaf;
			else
				scanState_ = ScanState::Idle;
			scanData_.clear();
			last_mean = 0;
		} else if (conf >= cfg_.confEpsilon && earlyTerminationByPhase(phase)) {
			scanState_ = ScanState::Settle;
			stepCount_ = (mode_ == AfModeContinuous) ? 0
								 : cfg_.speeds[speed_].stepFrames;
		} else
			doScan(contrast, phase, conf);
	}
}

void Af::updateLensPosition()
{
	if (scanState_ >= ScanState::Pdaf) {
		ftarget_ = std::clamp(ftarget_,
				      cfg_.ranges[range_].focusMin,
				      cfg_.ranges[range_].focusMax);
	}

	if (initted_) {
		/* from a known lens position: apply slew rate limit */
		fsmooth_ = std::clamp(ftarget_,
				      fsmooth_ - cfg_.speeds[speed_].maxSlew,
				      fsmooth_ + cfg_.speeds[speed_].maxSlew);
	} else {
		/* from an unknown position: go straight to target, but add delay */
		fsmooth_ = ftarget_;
		initted_ = true;
		skipCount_ = cfg_.skipFrames;
	}
}

void Af::startAF()
{
	/* Use PDAF if the tuning file allows it; else CDAF. */
	if (cfg_.speeds[speed_].dropoutFrames > 0 &&
	    (mode_ == AfModeContinuous || cfg_.speeds[speed_].pdafFrames > 0)) {
		if (!initted_) {
			ftarget_ = cfg_.ranges[range_].focusDefault;
			updateLensPosition();
		}
		stepCount_ = (mode_ == AfModeContinuous) ? 0 : cfg_.speeds[speed_].pdafFrames;
		scanState_ = ScanState::Pdaf;
		scanData_.clear();
		dropCount_ = 0;
		reportState_ = AfState::Scanning;
	} else
		startProgrammedScan();
}

void Af::startProgrammedScan()
{
	ftarget_ = cfg_.ranges[range_].focusMin;
	updateLensPosition();
	scanState_ = ScanState::Coarse;
	scanMaxContrast_ = 0.0;
	scanMinContrast_ = 1.0e9;
	scanMaxIndex_ = 0;
	scanData_.clear();
	stepCount_ = cfg_.speeds[speed_].stepFrames;
	reportState_ = AfState::Scanning;
	stable_frame_count = 0;
	last_mean = 0;
	trigger_when_stable = false;
	last_agc_status = false;
}

void Af::goIdle()
{
	scanState_ = ScanState::Idle;
	reportState_ = AfState::Idle;
	scanData_.clear();
}

/*
 * PDAF phase data are available in prepare(), but CDAF statistics are not
 * available until process(). We are gambling on the availability of PDAF.
 * To expedite feedback control using PDAF, issue the V4L2 lens control from
 * prepare(). Conversely, during scans, we must allow an extra frame delay
 * between steps, to retrieve CDAF statistics from the previous process()
 * so we can terminate the scan early without having to change our minds.
 */

void Af::prepare(Metadata *imageMetadata)
{
	/* Initialize for triggered scan or start of CAF mode */
	if (scanState_ == ScanState::Trigger)
		startAF();

	if (initted_) {
		/* Get PDAF from the embedded metadata, and run AF algorithm core */
		PdafRegions regions;
		double phase = 0.0, conf = 0.0;
		double oldFt = ftarget_;
		double oldFs = fsmooth_;
		ScanState oldSs = scanState_;
		uint32_t oldSt = stepCount_;
		imageMetadata_ = imageMetadata;
		if (imageMetadata->get("pdaf.regions", regions) == 0) {
			getPhase(regions, phase, conf);
			isPdafEnabled_ = true;
		}
		doAF(prevContrast_, phase, conf);
		updateLensPosition();
		LOG(RPiAf, Debug) << std::fixed << std::setprecision(2)
				  << static_cast<unsigned int>(reportState_)
				  << " sst" << static_cast<unsigned int>(oldSs)
				  << "->" << static_cast<unsigned int>(scanState_)
				  << " stp" << oldSt << "->" << stepCount_
				  << " ft" << oldFt << "->" << ftarget_
				  << " fs" << oldFs << "->" << fsmooth_
				  << " cont=" << (int)prevContrast_
				  << " phase=" << (int)phase << " conf=" << (int)conf;
	}

	/* Report status and produce new lens setting */
	AfStatus status;
	if (pauseFlag_)
		status.pauseState = (scanState_ == ScanState::Idle) ? AfPauseState::Paused
								    : AfPauseState::Pausing;
	else
		status.pauseState = AfPauseState::Running;

	if (mode_ == AfModeAuto && scanState_ != ScanState::Idle)
		status.state = AfState::Scanning;
	else
		status.state = reportState_;
	status.lensSetting = initted_ ? std::optional<int>(cfg_.map.eval(fsmooth_))
				      : std::nullopt;
	imageMetadata->set("af.status", status);
}

void Af::process(StatisticsPtr &stats, [[maybe_unused]] Metadata *imageMetadata)
{
	(void)imageMetadata;
	prevContrast_ = getContrast(stats->focusRegions);
	stats_ = stats;
}

/* Controls */

void Af::setRange(AfRange r)
{
	LOG(RPiAf, Debug) << "setRange: " << (unsigned)r;
	if (r < AfAlgorithm::AfRangeMax)
		range_ = r;
}

void Af::setSpeed(AfSpeed s)
{
	LOG(RPiAf, Debug) << "setSpeed: " << (unsigned)s;
	if (s < AfAlgorithm::AfSpeedMax) {
		if (scanState_ == ScanState::Pdaf &&
		    cfg_.speeds[s].pdafFrames > cfg_.speeds[speed_].pdafFrames)
			stepCount_ += cfg_.speeds[s].pdafFrames - cfg_.speeds[speed_].pdafFrames;
		speed_ = s;
	}
}

void Af::setMetering(bool mode)
{
	if (useWindows_ != mode) {
		useWindows_ = mode;
		invalidateWeights();
	}
}

void Af::setWindows(libcamera::Span<libcamera::Rectangle const> const &wins)
{
	windows_.clear();
	for (auto &w : wins) {
		LOG(RPiAf, Debug) << "Window: "
				  << w.x << ", "
				  << w.y << ", "
				  << w.width << ", "
				  << w.height;
		windows_.push_back(w);
		if (windows_.size() >= MaxWindows)
			break;
	}

	if (useWindows_)
		invalidateWeights();
}

bool Af::setLensPosition(double dioptres, int *hwpos)
{
	bool changed = false;

	if (mode_ == AfModeManual) {
		LOG(RPiAf, Debug) << "setLensPosition: " << dioptres;
		ftarget_ = cfg_.map.domain().clip(dioptres);
		changed = !(initted_ && fsmooth_ == ftarget_);
		updateLensPosition();
	}

	if (hwpos)
		*hwpos = cfg_.map.eval(fsmooth_);

	return changed;
}

std::optional<double> Af::getLensPosition() const
{
	/*
	 * \todo We ought to perform some precise timing here to determine
	 * the current lens position.
	 */
	return initted_ ? std::optional<double>(fsmooth_) : std::nullopt;
}

void Af::cancelScan()
{
	LOG(RPiAf, Debug) << "cancelScan";
	if (mode_ == AfModeAuto)
		goIdle();
}

void Af::triggerScan()
{
	LOG(RPiAf, Debug) << "triggerScan";
	if (mode_ == AfModeAuto && scanState_ == ScanState::Idle)
		scanState_ = ScanState::Trigger;
}

void Af::setMode(AfAlgorithm::AfMode mode)
{
	LOG(RPiAf, Debug) << "setMode: " << (unsigned)mode;
	if (mode_ != mode) {
		mode_ = mode;
		pauseFlag_ = false;
		if (mode == AfModeContinuous)
			scanState_ = ScanState::Trigger;
		else if (mode != AfModeAuto || scanState_ < ScanState::Coarse)
			goIdle();
	}
}

AfAlgorithm::AfMode Af::getMode() const
{
	return mode_;
}

void Af::pause(AfAlgorithm::AfPause pause)
{
	LOG(RPiAf, Debug) << "pause: " << (unsigned)pause;
	if (mode_ == AfModeContinuous) {
		if (pause == AfPauseResume && pauseFlag_) {
			pauseFlag_ = false;
			if (scanState_ < ScanState::Coarse)
				scanState_ = ScanState::Trigger;
		} else if (pause != AfPauseResume && !pauseFlag_) {
			pauseFlag_ = true;
			if (pause == AfPauseImmediate || scanState_ < ScanState::Coarse)
				goIdle();
		}
	}
}

// Register algorithm with the system.
static Algorithm *create(Controller *controller)
{
	return (Algorithm *)new Af(controller);
}
static RegisterAlgorithm reg(NAME, &create);
