function daily = ast_report_figures(csv_file, out_dir, varargin)
% AST_REPORT_FIGURES  Report-quality figures from the dashboard's raw CSV export.
%
%   ast_report_figures('G8_raw_20261120-20261127.csv', 'figures')
%   ast_report_figures(csv, out_dir, 'Day', '2026-11-21', 'DeltaTrk', 0.02, ...)
%   daily = ast_report_figures(...)     also returns the per-day table (struct)
%
% Desktop MATLAB (no toolboxes) or GNU Octave 8. Reads the raw-sample CSV that
% the phone dashboard exports (header in docs/PLAN.md), applies the metrics of
% docs/CONTRACT.md v1.5 section 6 (same numbers as analysis/py/ast_metrics.py)
% and writes each figure as PNG (300 dpi) and PDF into out_dir, plus
% report_daily_summary.csv with the numbers behind the daily figures:
%
%   fig1_power_<day>            P_trk, P_fix, P_use through one day
%   fig2_energy_<day>           cumulative energy with uncertainty bands
%   fig3_daily_net_gain         net gain per day with error bars
%   fig4_pointing_histogram     pointing-error histogram (contract 6.3 bins)
%   fig5_energy_budget          per day: fixed, extra from tracking, own use
%   fig6_calibration_mismatch   panel mismatch per calibration run (900-999)
%   fig7_completeness           samples received per day against the 99 % target
%
% Options (name, value):
%   'Day'       'YYYY-MM-DD' for figures 1-2; default: the tracking-test day
%               with the most tracked energy
%   'DeltaTrk'  relative uncertainty of the tracked-panel power  (default 0.02)
%   'DeltaFix'  relative uncertainty of the fixed-panel power    (default 0.02)
%   'DeltaUse'  relative uncertainty of the consumption power    (default 0.02)
%
% Uncertainty model (the group's analysis): each power channel has a relative
% scale uncertainty delta_P that is the same for every sample of a day, the
% three channels being independent. For the ratio r = E_trk / E_fix this gives
% delta_r / r = sqrt(delta_trk^2 + delta_fix^2) = sqrt(2) * delta_P for equal
% channels. First-order propagation to the gains, with u = E_use / E_fix:
%   gross = 100 (r - 1)       d_gross = 100 r sqrt(delta_trk^2 + delta_fix^2)
%   net   = 100 (r - 1 - u)   d_net   = 100 sqrt((r delta_trk)^2 + (u delta_use)^2
%                                                + ((r - u) delta_fix)^2)
% Calibration runs (both panels fixed) are never shown as a gain: their samples
% are left out of figures 1-5 and reported as a panel mismatch in figure 6.
% Samples marked simulated (sim = 1) put "SIMULATED DATA" on every figure.

    % ---------------------------------------------------------------- options
    if nargin < 2
        error('ast_report_figures:usage', 'usage: ast_report_figures(csv_file, out_dir, ...)');
    end
    opt = struct('Day', '', 'DeltaTrk', 0.02, 'DeltaFix', 0.02, 'DeltaUse', 0.02);
    for k = 1:2:numel(varargin)
        if ~isfield(opt, varargin{k})
            error('ast_report_figures:option', 'unknown option %s', varargin{k});
        end
        opt.(varargin{k}) = varargin{k + 1};
    end
    if ~exist(out_dir, 'dir')
        mkdir(out_dir);
    end

    % Okabe-Ito colour-blind-safe palette
    pal.blue = [0 114 178] / 255;      pal.vermillion = [213 94 0] / 255;
    pal.green = [0 158 115] / 255;     pal.orange = [230 159 0] / 255;
    pal.sky = [86 180 233] / 255;      pal.grey = [0.45 0.45 0.45];
    WH = 60 / 3600 / 1000;             % one 60 s sample of 1 mW in Wh (contract 6.2)
    EPOCH = 719529;                    % datenum of 1970-01-01
    TARGET_COMPLETE = 99;              % requirement F7 [%]

    % ------------------------------------------------------------ read the CSV
    d = read_raw_csv(csv_file);
    n_all = numel(d.t);
    if n_all == 0
        error('ast_report_figures:empty', 'no samples in %s', csv_file);
    end
    d.day = floor(d.t / 86400);                          % days since 1970-01-01 (UTC)
    is_cal = d.run >= 900 & d.run <= 999;                % calibration runs (contract 5)
    is_track = ismember(d.state, {'TRK', 'MOV', 'HLD'}); % states that count for pointing error
    err = d.ang_sun - d.ang_panel;                       % NaN when either angle is missing
    P = [d.p_trk, d.p_fix, d.p_use];
    P(isnan(P)) = 0;                                     % a missing power counts as 0

    % ------------------------------------------------------- per-day metrics
    days = (min(d.day):max(d.day))';
    nd = numel(days);
    di = d.day - days(1) + 1;
    keep_day = dedupe_keep(di, d.seq, d.entry_id, d.t);  % contract 6.1, inside each day
    daily.day = days;
    daily.date = cell(nd, 1);
    for i = 1:nd
        daily.date{i} = datestr(days(i) + EPOCH, 'yyyy-mm-dd');
    end
    daily.n_all = accumarray(di, 1, [nd, 1]);
    daily.n_unique = accumarray(di(keep_day), 1, [nd, 1]);
    daily.complete = 100 * daily.n_unique / 1440;        % every day in an export is treated as a past day
    daily.n_sim = accumarray(di(keep_day), double(d.sim(keep_day)), [nd, 1]);
    use = keep_day & ~is_cal;                            % samples behind the gain figures
    daily.n_test = accumarray(di(use), 1, [nd, 1]);
    daily.n_cal = accumarray(di(keep_day & is_cal), 1, [nd, 1]);
    daily.e_trk = accumarray(di(use), P(use, 1), [nd, 1]) * WH;
    daily.e_fix = accumarray(di(use), P(use, 2), [nd, 1]) * WH;
    daily.e_use = accumarray(di(use), P(use, 3), [nd, 1]) * WH;
    [daily.gross, daily.net, daily.d_gross, daily.d_net] = gains(daily.e_trk, daily.e_fix, daily.e_use, opt);
    pt = use & is_track & ~isnan(err);
    daily.n_point = accumarray(di(pt), 1, [nd, 1]);
    daily.mae = accumarray(di(pt), abs(err(pt)), [nd, 1]) ./ daily.n_point;
    daily.mae(daily.n_point == 0) = NaN;

    any_sim = any(d.sim);
    sim_note = '';
    if any_sim
        sim_note = sprintf('SIMULATED DATA (%d of %d samples) - not measurements', sum(d.sim), n_all);
    end
    day_labels = cell(nd, 1);
    for i = 1:nd
        day_labels{i} = datestr(days(i) + EPOCH, 'dd mmm');
    end
    period = sprintf('%s to %s', daily.date{1}, daily.date{end});

    % the day for figures 1 and 2
    if isempty(opt.Day)
        [~, pick] = max(daily.e_trk);
    else
        pick = find(strcmp(daily.date, opt.Day), 1);
        if isempty(pick)
            error('ast_report_figures:day', 'day %s is not in the file (%s)', opt.Day, period);
        end
    end
    sel = find(use & di == pick);
    [~, order] = sort(d.t(sel));
    sel = sel(order);
    hours = (d.t(sel) - days(pick) * 86400) / 3600;
    day_txt = daily.date{pick};
    xt = 0:3:24;
    xtl = {'00:00', '03:00', '06:00', '09:00', '12:00', '15:00', '18:00', '21:00', '24:00'};

    % ------------------------------------------------- figure 1: power in a day
    fig = new_figure();
    styles = {'-', '--', '-.'};  marks = {'o', 's', '^'};
    cols = {pal.blue, pal.vermillion, pal.green};
    names = {'Tracked panel', 'Fixed panel', 'Tracker own use'};
    vals = [d.p_trk(sel), d.p_fix(sel), d.p_use(sel)];
    h = zeros(1, 3);
    [hx, pos] = with_breaks(hours);
    for j = 1:3
        y = NaN(size(hx));
        y(pos) = vals(:, j);
        plot(hx, y, 'LineStyle', styles{j}, 'Color', cols{j}, 'LineWidth', 1.2);
        mk = (1 + (j - 1) * 20):60:numel(sel);
        plot(hours(mk), vals(mk, j), 'LineStyle', 'none', 'Marker', marks{j}, 'Color', cols{j}, ...
             'MarkerFaceColor', 'w', 'MarkerSize', 4.5);
        h(j) = plot(NaN, NaN, 'LineStyle', styles{j}, 'Marker', marks{j}, 'Color', cols{j}, ...
                    'MarkerFaceColor', 'w', 'MarkerSize', 4.5, 'LineWidth', 1.2);
    end
    set(gca, 'XLim', [0 24], 'XTick', xt, 'XTickLabel', xtl);
    top = max(vals(:));
    if ~(top > 0), top = 1; end
    set(gca, 'YLim', [0, 1.08 * top]);
    xlabel('Time of day (UTC)');
    ylabel('Mean power over one minute (mW)');
    legend(h, names, 'Location', 'northeast');
    finish(fig, {sprintf('Panel power on %s (UTC)', day_txt)}, sim_note, out_dir, ['fig1_power_', day_txt]);

    % ------------------------------------- figure 2: cumulative energy in a day
    fig = new_figure();
    E = cumsum(P(sel, :), 1) * WH;
    net_curve = E(:, 1) - E(:, 3);
    d_net_curve = sqrt((E(:, 1) * opt.DeltaTrk) .^ 2 + (E(:, 3) * opt.DeltaUse) .^ 2);
    band(hours, E(:, 2) * (1 - opt.DeltaFix), E(:, 2) * (1 + opt.DeltaFix), tint(pal.vermillion));
    band(hours, net_curve - d_net_curve, net_curve + d_net_curve, tint(pal.green));
    series = [E(:, 1), E(:, 2), net_curve];
    names = {sprintf('Tracked panel: %.3g Wh', E(end, 1)), sprintf('Fixed panel: %.3g Wh', E(end, 2)), ...
             sprintf('Tracked minus own use: %.3g Wh', net_curve(end))};
    cols = {pal.blue, pal.vermillion, pal.green};
    h = zeros(1, 3);
    for j = 1:3
        plot(hours, series(:, j), 'LineStyle', styles{j}, 'Color', cols{j}, 'LineWidth', 1.2);
        mk = (1 + (j - 1) * 20):60:numel(sel);
        plot(hours(mk), series(mk, j), 'LineStyle', 'none', 'Marker', marks{j}, 'Color', cols{j}, ...
             'MarkerFaceColor', 'w', 'MarkerSize', 4.5);
        h(j) = plot(NaN, NaN, 'LineStyle', styles{j}, 'Marker', marks{j}, 'Color', cols{j}, ...
                    'MarkerFaceColor', 'w', 'MarkerSize', 4.5, 'LineWidth', 1.2);
    end
    set(gca, 'XLim', [0 24], 'XTick', xt, 'XTickLabel', xtl);
    lo = min(0, 1.3 * min(net_curve - d_net_curve));
    set(gca, 'YLim', [lo, max(1.08 * max(series(:)), lo + 0.001)]);
    xlabel('Time of day (UTC)');
    ylabel('Energy since 00:00 UTC (Wh)');
    legend(h, names, 'Location', 'northwest');
    head = {sprintf('Cumulative energy on %s (UTC)', day_txt)};
    if ~isnan(daily.net(pick))
        head{2} = sprintf('Net gain %+.1f %% +/- %.1f %% (gross %+.1f %%); bands: +/- %g %% fixed, +/- %g %% tracked', ...
                          daily.net(pick), daily.d_net(pick), daily.gross(pick), 100 * opt.DeltaFix, 100 * opt.DeltaTrk);
    end
    finish(fig, head, sim_note, out_dir, ['fig2_energy_', day_txt]);

    % ------------------------------------------- figure 3: daily net gain
    fig = new_figure();
    x = (1:nd)';
    has = ~isnan(daily.net);
    lo = min([0; daily.net(has) - daily.d_net(has)]);
    hi = max([0; daily.net(has) + daily.d_net(has)]);
    span = max(hi - lo, 2);
    yl = [lo - 0.16 * span, hi + 0.16 * span];
    % all tracking-test samples together
    [~, net_all, ~, d_net_all] = gains(sum(daily.e_trk), sum(daily.e_fix), sum(daily.e_use), opt);
    h_all = [];
    if ~isnan(net_all)
        band([0.4; nd + 0.6], (net_all - d_net_all) * [1; 1], (net_all + d_net_all) * [1; 1], [0.88 0.88 0.88]);
        h_all = plot([0.4, nd + 0.6], net_all * [1 1], '--', 'Color', pal.grey, 'LineWidth', 1);
    end
    h_gain = [];  h_loss = [];
    for i = 1:nd
        v = daily.net(i);
        if isnan(v)
            why = 'no test data';
            if daily.n_test(i) > 0, why = 'n/a'; end
            if daily.n_cal(i) > 0 && daily.n_test(i) == 0, why = 'calibration day'; end
            text(i, 0.02 * span, why, 'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom', ...
                 'FontSize', 7, 'Color', [0.3 0.3 0.3], 'Rotation', 90);
            continue
        end
        if v < 0
            h_loss = bar_patch(i, 0.36, v, 0, pal.vermillion, diff(yl) / 30);
        else
            h_gain = bar_patch(i, 0.36, 0, v, pal.blue, 0);
        end
        plot([i i], v + daily.d_net(i) * [-1 1], '-k', 'LineWidth', 1);
        plot(i + 0.12 * [-1 1], (v - daily.d_net(i)) * [1 1], '-k', 'LineWidth', 1);
        plot(i + 0.12 * [-1 1], (v + daily.d_net(i)) * [1 1], '-k', 'LineWidth', 1);
        if v < 0
            text(i, v - daily.d_net(i) - 0.015 * span, sprintf('%+.1f', v), 'HorizontalAlignment', 'center', ...
                 'VerticalAlignment', 'top', 'FontSize', 8);
        else
            text(i, v + daily.d_net(i) + 0.015 * span, sprintf('%+.1f', v), 'HorizontalAlignment', 'center', ...
                 'VerticalAlignment', 'bottom', 'FontSize', 8);
        end
    end
    plot([0.4, nd + 0.6], [0 0], '-k', 'LineWidth', 0.8);
    if isempty(h_gain), h_gain = bar_patch(-5, 0.3, 0, 0, pal.blue, 0); end
    if isempty(h_loss), h_loss = bar_patch(-5, 0.3, 0, 0, pal.vermillion, 0); end
    hs = [h_gain, h_loss];
    ls = {'Net gain (+)', 'Net loss (-), hatched'};
    if ~isempty(h_all)
        hs(end + 1) = h_all;
        ls{end + 1} = sprintf('All test days: %+.1f +/- %.1f %%', net_all, d_net_all);
    end
    set(gca, 'XLim', [0.4, nd + 0.6], 'XTick', x, 'XTickLabel', day_labels, 'YLim', yl, 'YGrid', 'on');
    xlabel('UTC day');
    ylabel('Net energy gain (%)');
    legend(hs, ls, 'Location', 'southoutside', 'Orientation', 'horizontal');
    finish(fig, {sprintf('Daily net energy gain of the tracked panel, %s', period), ...
                 sprintf('Error bars: power uncertainty +/- %g %% tracked, +/- %g %% fixed, +/- %g %% own use', ...
                         100 * opt.DeltaTrk, 100 * opt.DeltaFix, 100 * opt.DeltaUse)}, ...
           sim_note, out_dir, 'fig3_daily_net_gain');

    % ------------------------------------ figure 4: pointing-error histogram
    fig = new_figure();
    e = err(pt);
    bin = floor(e + 30) + 2;                             % bins 2..61 = [-30,-29) .. [29,30)
    bin(e < -30) = 1;                                    % underflow
    bin(e >= 30) = 62;                                   % overflow
    counts = accumarray(bin, 1, [62, 1]);
    centres = [-31.5, -29.5:1:29.5, 31.5];
    for b = 1:62
        if counts(b) > 0
            colour = pal.blue;
            if b == 1 || b == 62, colour = pal.orange; end
            patch('XData', centres(b) + 0.5 * [-1 1 1 -1], 'YData', [0 0 counts(b) counts(b)], ...
                  'FaceColor', colour, 'EdgeColor', 'k', 'LineWidth', 0.4);
        end
    end
    top = max([counts; 1]);
    plot([0 0], [0, 1.1 * top], ':', 'Color', pal.grey, 'LineWidth', 1);
    set(gca, 'XLim', [-33 33], 'XTick', [-31.5, -20:10:20, 31.5], ...
        'XTickLabel', {'< -30', '-20', '-10', '0', '10', '20', '>= 30'}, 'YLim', [0, 1.1 * top], 'YGrid', 'on');
    xlabel('Pointing error, sun angle minus panel angle (degrees), 1-degree bins');
    ylabel('Samples (minutes)');
    head = {sprintf('Pointing error while tracking (orange: outside +/- 30 deg), %s', period)};
    if isempty(e)
        head{2} = 'No pointing samples (sun angle unknown or tracker not tracking)';
    else
        head{2} = sprintf('Mean absolute error %.2f deg, %d samples in states TRK, MOV, HLD; %.1f %% within +/- 5 deg', ...
                          mean(abs(e)), numel(e), 100 * mean(abs(e) <= 5));
    end
    finish(fig, head, sim_note, out_dir, 'fig4_pointing_histogram');

    % --------------------------------------------- figure 5: energy budget
    fig = new_figure();
    extra = daily.e_trk - daily.e_fix;
    top = max([daily.e_trk; daily.e_fix; 0.001]);
    low = max([daily.e_use; 0]);
    yl = [-(low + 0.12 * top), 1.22 * top];
    h1 = [];  h2 = [];  h3 = [];
    for i = 1:nd
        if daily.n_test(i) == 0
            why = 'no test data';
            if daily.n_cal(i) > 0, why = 'calibration day'; end
            text(i, 0.02 * top, why, 'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom', ...
                 'FontSize', 7, 'Color', [0.3 0.3 0.3], 'Rotation', 90);
            continue
        end
        h1 = bar_patch(i, 0.36, 0, daily.e_fix(i), pal.sky, 0);
        if extra(i) > 0
            h2 = bar_patch(i, 0.36, daily.e_fix(i), daily.e_trk(i), pal.blue, 0);
        end
        if daily.e_use(i) > 0
            h3 = bar_patch(i, 0.36, -daily.e_use(i), 0, pal.vermillion, diff(yl) / 40);
        end
        label = sprintf('%+.2f', extra(i) - daily.e_use(i));
        if extra(i) < 0
            label = {label, 'trk < fix'};
        end
        text(i, max(daily.e_trk(i), daily.e_fix(i)) + 0.02 * top, label, 'HorizontalAlignment', 'center', ...
             'VerticalAlignment', 'bottom', 'FontSize', 8);
    end
    plot([0.4, nd + 0.6], [0 0], '-k', 'LineWidth', 0.8);
    if isempty(h1), h1 = bar_patch(-5, 0.3, 0, 0, pal.sky, 0); end
    if isempty(h2), h2 = bar_patch(-5, 0.3, 0, 0, pal.blue, 0); end
    if isempty(h3), h3 = bar_patch(-5, 0.3, 0, 0, pal.vermillion, 0); end
    set(gca, 'XLim', [0.4, nd + 0.6], 'XTick', x, 'XTickLabel', day_labels, 'YLim', yl, 'YGrid', 'on');
    xlabel('UTC day');
    ylabel('Energy (Wh)');
    legend([h1, h2, h3], {'Fixed panel', 'Extra from tracking', 'Tracker own use (below zero, hatched)'}, ...
           'Location', 'southoutside', 'Orientation', 'horizontal');
    finish(fig, {sprintf('Energy budget per day, %s', period), ...
                 'Number above each bar: net energy gained = extra from tracking minus own use (Wh)'}, ...
           sim_note, out_dir, 'fig5_energy_budget');

    % ----------------------------------- figure 6: calibration-run mismatch
    fig = new_figure();
    cal_rows = find(is_cal);
    runs = zeros(0, 1);
    if ~isempty(cal_rows)
        % one group per (run, panel letter), duplicates removed inside each group
        panel_code = cellfun(@(p) sum(double(p) .* 256 .^ (numel(p) - 1:-1:0)), d.panel(cal_rows));
        [groups, ~, gi] = unique([d.run(cal_rows), panel_code], 'rows');
        keep_cal = dedupe_keep(gi, d.seq(cal_rows), d.entry_id(cal_rows), d.t(cal_rows));
        rows = cal_rows(keep_cal);
        gi = gi(keep_cal);
        ng = size(groups, 1);
        runs = groups(:, 1);
        c_trk = accumarray(gi, P(rows, 1), [ng, 1]) * WH;
        c_fix = accumarray(gi, P(rows, 2), [ng, 1]) * WH;
        [mism, ~, d_mism] = gains(c_trk, c_fix, zeros(ng, 1), opt);
        span = max([abs(mism) + d_mism; 1]);
        labels = cell(ng, 1);
        for g = 1:ng
            first_row = rows(find(gi == g, 1));
            panel = d.panel{first_row};
            if isempty(panel), panel = '?'; end
            labels{g} = sprintf('run %d (%s)', runs(g), panel);
            if isnan(mism(g))
                text(g, 0, 'n/a', 'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom', 'FontSize', 8);
                continue
            end
            if mism(g) < 0
                bar_patch(g, 0.3, mism(g), 0, pal.orange, 2.6 * span / 30);
                va = 'top';  off = -d_mism(g) - 0.03 * span;
            else
                bar_patch(g, 0.3, 0, mism(g), pal.orange, 0);
                va = 'bottom';  off = d_mism(g) + 0.03 * span;
            end
            plot([g g], mism(g) + d_mism(g) * [-1 1], '-k', 'LineWidth', 1);
            plot(g + 0.1 * [-1 1], (mism(g) - d_mism(g)) * [1 1], '-k', 'LineWidth', 1);
            plot(g + 0.1 * [-1 1], (mism(g) + d_mism(g)) * [1 1], '-k', 'LineWidth', 1);
            text(g, mism(g) + off, sprintf('%+.2f %%', mism(g)), 'HorizontalAlignment', 'center', ...
                 'VerticalAlignment', va, 'FontSize', 8);
        end
        plot([0.4, ng + 0.6], [0 0], '-k', 'LineWidth', 0.8);
        set(gca, 'XLim', [0.4, ng + 0.6], 'XTick', 1:ng, 'XTickLabel', labels, 'YLim', 1.3 * span * [-1 1], 'YGrid', 'on');
        xlabel('Calibration run (panel on the tracker mount)');
        ylabel('Mismatch 100 (E_{trk} - E_{fix}) / E_{fix} (%)');
    else
        axis([0 1 0 1]);
        axis off
        text(0.5, 0.5, {'No calibration runs in this file', '(run IDs 900 to 999: both panels fixed side by side)'}, ...
             'HorizontalAlignment', 'center', 'FontSize', 11);
    end
    finish(fig, {sprintf('Panel mismatch in calibration runs, %s', period), ...
                 sprintf('Both panels fixed: a panel difference, not a gain. Error bars +/- %.1f %%', ...
                         100 * sqrt(opt.DeltaTrk ^ 2 + opt.DeltaFix ^ 2))}, ...
           sim_note, out_dir, 'fig6_calibration_mismatch');

    % ---------------------------------------------- figure 7: completeness
    fig = new_figure();
    yl = [min(90, 5 * floor(min(daily.complete) / 5) - 5), 102];
    yl(1) = max(yl(1), 0);
    for i = 1:nd
        if daily.complete(i) >= TARGET_COMPLETE
            bar_patch(i, 0.36, yl(1), daily.complete(i), pal.blue, 0);
        else
            bar_patch(i, 0.36, yl(1), max(daily.complete(i), yl(1)), pal.vermillion, diff(yl) / 30);
        end
        text(i, daily.complete(i) + 0.01 * diff(yl), sprintf('%.1f', daily.complete(i)), ...
             'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom', 'FontSize', 8);
    end
    h_ok = bar_patch(-5, 0.3, yl(1), yl(1), pal.blue, 0);
    h_bad = bar_patch(-5, 0.3, yl(1), yl(1), pal.vermillion, 0);
    h_t = plot([0.4, nd + 0.6], TARGET_COMPLETE * [1 1], '--k', 'LineWidth', 1);
    set(gca, 'XLim', [0.4, nd + 0.6], 'XTick', x, 'XTickLabel', day_labels, 'YLim', yl, 'YGrid', 'on');
    xlabel('UTC day');
    ylabel('Completeness, unique samples / 1440 (%)');
    legend([h_ok, h_bad, h_t], {'Target met', 'Below target (hatched)', sprintf('Target %d %% (F7)', TARGET_COMPLETE)}, ...
           'Location', 'southoutside', 'Orientation', 'horizontal');
    finish(fig, {sprintf('Data completeness per day, %s', period), ...
                 sprintf('%d samples received, %d duplicates removed', n_all, n_all - sum(daily.n_unique))}, ...
           sim_note, out_dir, 'fig7_completeness');

    % ------------------------------------------------------ summary table
    fid = fopen(fullfile(out_dir, 'report_daily_summary.csv'), 'w');
    fprintf(fid, ['date,n_received,n_unique,complete_pct,n_sim,n_test,e_trk_wh,e_fix_wh,e_use_wh,' ...
                  'gain_gross_pct,gain_net_pct,gain_net_unc_pct,mae_point_deg\n']);
    for i = 1:nd
        fprintf(fid, '%s,%d,%d,%.4f,%d,%d,%.8g,%.8g,%.8g,%s,%s,%s,%s\n', daily.date{i}, daily.n_all(i), ...
                daily.n_unique(i), daily.complete(i), daily.n_sim(i), daily.n_test(i), daily.e_trk(i), ...
                daily.e_fix(i), daily.e_use(i), num_or_empty(daily.gross(i)), num_or_empty(daily.net(i)), ...
                num_or_empty(daily.d_net(i)), num_or_empty(daily.mae(i)));
    end
    fclose(fid);
    fprintf('ast_report_figures: %d samples, %d days (%s), 7 figures (PNG + PDF) and report_daily_summary.csv in %s\n', ...
            n_all, nd, period, out_dir);
    if any_sim
        fprintf('NOTE: %d of %d samples are SIMULATED (sim = 1); every figure says so.\n', sum(d.sim), n_all);
    end
end

% ======================================================================
% local functions
% ======================================================================

function d = read_raw_csv(csv_file)
% Dashboard raw export -> struct of columns. Numbers: NaN when empty.
    header = {'created_at_utc', 'time_uk', 'entry_id', 'seq', 'p_trk_mw', 'p_fix_mw', 'p_use_mw', 'ang_panel_deg', ...
              'ang_sun_deg', 'temp_c', 'vbat_v', 'state', 'run', 'panel', 'fault', 'sim', 'status'};
    txt = fileread(csv_file);
    if ~isempty(txt) && double(txt(1)) == 65279
        txt = txt(2:end);                                % byte-order mark
    end
    if numel(txt) >= 3 && isequal(double(txt(1:3)), [239 187 191])
        txt = txt(4:end);                                % UTF-8 byte-order mark read as bytes
    end
    lines = regexp(txt, '\r?\n', 'split');
    lines = lines(~cellfun('isempty', lines));
    if isempty(lines) || ~strcmp(strtrim(lines{1}), strjoin(header, ','))
        error('ast_report_figures:header', '%s is not a dashboard raw export (unexpected header line)', csv_file);
    end
    if any(cellfun(@(s) any(s == '"'), lines))
        error('ast_report_figures:quotes', 'quoted CSV cells are not supported (none are expected in a raw export)');
    end
    cells = regexp(lines(2:end), ',', 'split');
    width = cellfun('length', cells);
    if any(width ~= numel(header))
        error('ast_report_figures:columns', 'line %d has %d columns, expected %d', ...
              find(width ~= numel(header), 1) + 1, width(find(width ~= numel(header), 1)), numel(header));
    end
    c = vertcat(cells{:});
    if isempty(c)
        c = cell(0, numel(header));
    end
    % created_at_utc 'YYYY-MM-DDTHH:MM:SSZ' -> POSIX seconds
    n = size(c, 1);
    d.t = zeros(n, 1);
    if n > 0
        T = char(c(:, 1));
        D = double(T(:, 1:19)) - 48;
        d.t = (datenum(D(:, 1:4) * [1000; 100; 10; 1], D(:, 6:7) * [10; 1], D(:, 9:10) * [10; 1]) - 719529) * 86400 ...
              + D(:, 12:13) * [36000; 3600] + D(:, 15:16) * [600; 60] + D(:, 18:19) * [10; 1];
    end
    d.entry_id = str2double(c(:, 3));
    d.seq = fix(str2double(c(:, 4)));
    d.p_trk = str2double(c(:, 5));
    d.p_fix = str2double(c(:, 6));
    d.p_use = str2double(c(:, 7));
    d.ang_panel = str2double(c(:, 8));
    d.ang_sun = str2double(c(:, 9));
    d.state = c(:, 12);
    d.run = str2double(c(:, 13));
    d.run(isnan(d.run)) = 0;
    d.panel = c(:, 14);
    d.sim = strcmp(c(:, 16), '1');
end

function keep = dedupe_keep(group, seq, entry_id, t)
% Contract 6.1 inside each group: same seq AND same time = a re-sent copy; keep
% the first received copy (lowest entry_id); rows without a seq are all kept.
    keep = true(size(seq));
    rank = entry_id;
    rank(isnan(rank)) = Inf;
    idx = find(~isnan(seq));
    if isempty(idx)
        return
    end
    [~, order] = sortrows([group(idx), seq(idx), t(idx), rank(idx)]);
    s = idx(order);
    dup = [false; group(s(2:end)) == group(s(1:end - 1)) & seq(s(2:end)) == seq(s(1:end - 1)) ...
           & t(s(2:end)) == t(s(1:end - 1))];
    keep(s(dup)) = false;
end

function [gross, net, d_gross, d_net] = gains(e_trk, e_fix, e_use, opt)
% Contract 6.2 gains [%] and their uncertainty (see the file header); NaN when E_fix <= 0.
    gross = NaN(size(e_fix));  net = gross;  d_gross = gross;  d_net = gross;
    ok = e_fix > 0;
    r = e_trk(ok) ./ e_fix(ok);
    u = e_use(ok) ./ e_fix(ok);
    gross(ok) = 100 * (e_trk(ok) - e_fix(ok)) ./ e_fix(ok);
    net(ok) = 100 * (e_trk(ok) - e_fix(ok) - e_use(ok)) ./ e_fix(ok);
    d_gross(ok) = 100 * r * sqrt(opt.DeltaTrk ^ 2 + opt.DeltaFix ^ 2);
    d_net(ok) = 100 * sqrt((r * opt.DeltaTrk) .^ 2 + (u * opt.DeltaUse) .^ 2 + ((r - u) * opt.DeltaFix) .^ 2);
end

function fig = new_figure()
% 16 cm x 10 cm figure (fits a report column at full width), hidden while drawing.
    fig = figure('Visible', 'off', 'Color', 'w', 'Units', 'pixels', 'Position', [50 50 800 500]);
    set(fig, 'PaperUnits', 'centimeters', 'PaperSize', [16 10], 'PaperPosition', [0 0 16 10]);
    axes('FontSize', 9, 'Box', 'on', 'XGrid', 'off', 'YGrid', 'on', 'Layer', 'top');
    hold on
end

function finish(fig, heading, sim_note, out_dir, name)
% Title (with the SIMULATED DATA line when it applies), then PNG at 300 dpi and PDF.
    if ~isempty(sim_note)
        heading{end + 1} = sim_note;
    end
    title(heading, 'FontSize', 9, 'Interpreter', 'none');
    hold off
    print(fig, '-dpng', '-r300', fullfile(out_dir, [name, '.png']));
    print(fig, '-dpdf', fullfile(out_dir, [name, '.pdf']));
    close(fig);
end

function [hx, pos] = with_breaks(hours)
% x values with a NaN after every gap longer than 1.5 minutes; pos = where the samples go.
    n = numel(hours);
    brk = zeros(n, 1);
    brk(diff(hours) > 1.5 / 60) = 1;
    pos = (1:n)' + [0; cumsum(brk(1:end - 1))];
    hx = NaN(n + sum(brk), 1);
    hx(pos) = hours;
end

function band(x, lower, upper, colour)
% Filled uncertainty band between two curves.
    x = x(:);  lower = lower(:);  upper = upper(:);
    patch('XData', [x; flipud(x)], 'YData', [lower; flipud(upper)], 'FaceColor', colour, 'EdgeColor', 'none');
end

function c = tint(colour)
% Light version of a palette colour for bands.
    c = 1 - 0.28 * (1 - colour);
end

function h = bar_patch(x, half, y0, y1, colour, hatch_step)
% One bar from y0 to y1. hatch_step > 0 adds white rising hatch lines, so a
% bar class is recognisable without colour.
    h = patch('XData', x + half * [-1 1 1 -1], 'YData', [y0 y0 y1 y1], 'FaceColor', colour, ...
              'EdgeColor', 'k', 'LineWidth', 0.6);
    if hatch_step > 0 && y1 > y0
        rise = 2.5 * hatch_step;
        hx = [];  hy = [];
        for ya = (y0 - rise):hatch_step:y1
            ta = max(0, (y0 - ya) / rise);
            tb = min(1, (y1 - ya) / rise);
            if ta < tb
                hx = [hx, x - half + 2 * half * [ta, tb], NaN]; %#ok<AGROW>
                hy = [hy, ya + rise * [ta, tb], NaN]; %#ok<AGROW>
            end
        end
        plot(hx, hy, '-', 'Color', 'w', 'LineWidth', 0.9);
    end
end

function s = num_or_empty(v)
% Number for the summary CSV, empty when undefined.
    if isnan(v)
        s = '';
    else
        s = sprintf('%.6f', v);
    end
end
