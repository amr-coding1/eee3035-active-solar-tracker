% VIS_DAILY_NET_GAIN  Net energy gain of the tracked panel, one bar per UTC day.
%
% ThingSpeak "MATLAB Visualization" for the Active Solar Tracker (EEE3035
% Group 8). Reads channel 2 ("AST results", contract v1.5 section 3.2), keeps
% the newest row for every summarised date and draws the net gain
%     100 * (E_trk - E_fix - E_use) / E_fix   [%]
% for the 14 UTC days that end with the newest summarised day.
% No toolboxes; also runs in GNU Octave 8.
%
% Accessibility: gain and loss differ in colour (Okabe-Ito blue / vermillion)
% AND in pattern (loss bars are hatched) AND every bar carries its signed
% value as text, so colour is never the only cue. Days with no result say
% "n/a". Days that include simulated samples are marked "sim", days less than
% 99 % complete are marked "part".

% ------------------------------------------------------------------------
% CONFIGURATION (ThingSpeak: Apps > MATLAB Visualizations > New > Custom)
% ------------------------------------------------------------------------
CH2_ID       = 3578278;  % channel 2 "AST results" ID
CH2_READ_KEY = '';       % channel 2 Read API key; '' = public channel (no key sent)
BASE_URL     = 'https://api.thingspeak.com';
NBARS        = 14;       % days shown
COMPLETE_MIN = 99;       % a day below this completeness (%) is marked "part" (requirement F7)

% Test overrides (a test harness sets these variables before run(...))
if exist('AST_BASE_URL', 'var'),     BASE_URL = AST_BASE_URL;         end
if exist('AST_CH2_ID', 'var'),       CH2_ID = AST_CH2_ID;             end
if exist('AST_CH2_READ_KEY', 'var'), CH2_READ_KEY = AST_CH2_READ_KEY; end

% Okabe-Ito colour-blind-safe palette
C_BLUE = [0 114 178] / 255;
C_VERMILLION = [213 94 0] / 255;
FONT_SIZE = 12;
EPOCH_DATENUM = 719529;                  % datenum of 1970-01-01

% ------------------------------------------------------------------------
% Read channel 2: the newest 400 rows cover more than 8 weeks of nightly runs
% ------------------------------------------------------------------------
key_param = '';
if ~isempty(CH2_READ_KEY)
    key_param = ['api_key=', CH2_READ_KEY, '&'];
end
url = sprintf('%s/channels/%s/feeds.json?%sresults=400&status=true&timezone=Etc/UTC', BASE_URL, num2str(CH2_ID), key_param);
reply = webread(url, weboptions('Timeout', 15));
if ischar(reply), reply = jsondecode(reply); end            % Octave returns text
feeds = [];
if isstruct(reply) && isfield(reply, 'feeds'), feeds = reply.feeds; end

names = {'created_at', 'entry_id', 'status', 'field5', 'field7'};
m = numel(feeds);
cols = cell(m, numel(names));
for k = 1:numel(names)
    if isstruct(feeds)                                       % struct array, or cell array of structs
        if isfield(feeds, names{k}), cols(:, k) = reshape({feeds.(names{k})}, m, 1); end
    elseif iscell(feeds)
        for i = 1:m
            if isfield(feeds{i}, names{k}), cols{i, k} = feeds{i}.(names{k}); end
        end
    end
end

% keep 'period=day;date=YYYY-MM-DD' rows with a readable created_at
status = cols(:, 3);
status(~cellfun(@ischar, status)) = {''};
date_txt = regexp(status, ';date=\d{4}-\d{2}-\d{2}', 'match', 'once');    % ';date=YYYY-MM-DD' or ''
date_txt(~cellfun(@ischar, date_txt)) = {''};
date_txt = cellfun(@(c) c(min(7, end + 1):end), date_txt, 'UniformOutput', false);   % drop ';date='
is_day = ~cellfun('isempty', regexp(status, '(^|;)period=day(;|$)', 'match', 'once'));
ok = is_day & ~cellfun('isempty', date_txt) & cellfun(@(c) ischar(c) && numel(c) >= 19, cols(:, 1));
cols = cols(ok, :);  status = status(ok);  date_txt = date_txt(ok);
m = size(cols, 1);

net = NaN(1, NBARS);  complete = NaN(1, NBARS);  n_sim = zeros(1, NBARS);  has_row = false(1, NBARS);
day_no = [];
if m > 0
    D = double(char(date_txt)) - 48;                         % summarised date -> day number
    row_day = datenum(D(:, 1:4) * [1000; 100; 10; 1], D(:, 6:7) * [10; 1], D(:, 9:10) * [10; 1]) - EPOCH_DATENUM;
    T = char(cols(:, 1));                                    % created_at 'YYYY-MM-DDTHH:MM:SSZ' -> seconds
    D = double(T(:, 1:19)) - 48;
    written = (datenum(D(:, 1:4) * [1000; 100; 10; 1], D(:, 6:7) * [10; 1], D(:, 9:10) * [10; 1]) - EPOCH_DATENUM) * 86400 ...
              + D(:, 12:13) * [36000; 3600] + D(:, 15:16) * [600; 60] + D(:, 18:19) * [10; 1];
    entry_id = zeros(m, 1);
    has_id = cellfun(@(c) isnumeric(c) && isscalar(c), cols(:, 2));
    entry_id(has_id) = cellfun(@double, cols(has_id, 2));
    % newest row per date (contract 3.2): sort by date, then created_at, then entry_id; take the last of each date
    [~, order] = sortrows([row_day, written, entry_id]);
    newest = order([row_day(order(2:end)) ~= row_day(order(1:end - 1)); true]);
    sim_txt = regexp(status, ';ns=\d+', 'match', 'once');                 % ';ns=<count>' or ''
    sim_txt(~cellfun(@ischar, sim_txt)) = {''};
    sim_txt = cellfun(@(c) c(min(5, end + 1):end), sim_txt, 'UniformOutput', false);
    day_no = (max(row_day) - NBARS + 1):max(row_day);        % the NBARS calendar days shown
    for i = newest'
        b = row_day(i) - day_no(1) + 1;
        if b >= 1
            has_row(b) = true;
            net(b) = str2double(cols{i, 4});                 % field5; null (E_fix <= 0) -> NaN
            complete(b) = str2double(cols{i, 5});            % field7
            n_sim(b) = max(0, str2double(sim_txt{i}));       % NaN (no ns in the status) -> 0
        end
    end
end

% ------------------------------------------------------------------------
% Draw
% ------------------------------------------------------------------------
figure('Color', 'w');
axes('FontSize', FONT_SIZE);
hold on
if isempty(day_no)
    axis([0 1 0 1]);
    axis off
    text(0.5, 0.5, {'No data', 'Channel 2 has no daily results yet.', 'The nightly job writes them at 00:20 UTC.'}, ...
         'HorizontalAlignment', 'center', 'FontSize', FONT_SIZE + 2);
    title('Daily net energy gain', 'FontSize', FONT_SIZE + 1, 'Visible', 'on');
else
    lo = min([0, net(~isnan(net))]);
    hi = max([0, net(~isnan(net))]);
    span = max(hi - lo, 2);
    y_lim = [lo - 0.22 * span, hi + 0.22 * span];            % room for the value labels
    half = 0.38;                                             % half bar width (days)
    hatch_step = diff(y_lim) / 28;                           % vertical distance between hatch lines
    hatch_rise = 2.5 * hatch_step;                           % rise of a hatch line across one bar
    hx = [];  hy = [];
    for b = 1:NBARS
        v = net(b);
        if isnan(v)
            why = 'n/a';
            if ~has_row(b), why = 'no row'; end
            text(b, 0.02 * span, why, 'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom', ...
                 'FontSize', FONT_SIZE - 3, 'Color', [0.3 0.3 0.3]);
            continue
        end
        x0 = b - half;  x1 = b + half;
        if round(v * 10) < 0
            patch('XData', [x0 x1 x1 x0], 'YData', [v v 0 0], 'FaceColor', C_VERMILLION, 'EdgeColor', 'k', 'LineWidth', 1);
            for ya = (v - hatch_rise):hatch_step:0           % hatch: rising lines clipped to the bar
                ta = max(0, (v - ya) / hatch_rise);
                tb = min(1, (0 - ya) / hatch_rise);
                if ta < tb
                    hx = [hx, x0 + ta * (x1 - x0), x0 + tb * (x1 - x0), NaN]; %#ok<AGROW>
                    hy = [hy, ya + ta * hatch_rise, ya + tb * hatch_rise, NaN]; %#ok<AGROW>
                end
            end
        else
            patch('XData', [x0 x1 x1 x0], 'YData', [0 0 v v], 'FaceColor', C_BLUE, 'EdgeColor', 'k', 'LineWidth', 1);
        end
        label = {sprintf('%+.1f', v)};
        if round(v * 10) == 0, label = {'0.0'}; end
        if n_sim(b) > 0, label{end + 1} = 'sim'; end %#ok<SAGROW>
        if complete(b) < COMPLETE_MIN, label{end + 1} = 'part'; end %#ok<SAGROW>
        if round(v * 10) < 0
            text(b, v - 0.015 * span, label, 'HorizontalAlignment', 'center', 'VerticalAlignment', 'top', 'FontSize', FONT_SIZE - 3);
        else
            text(b, v + 0.015 * span, label, 'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom', 'FontSize', FONT_SIZE - 3);
        end
    end
    if ~isempty(hx)
        plot(hx, hy, '-', 'Color', 'w', 'LineWidth', 1.2);
    end
    plot([0.4, NBARS + 0.6], [0 0], '-', 'Color', 'k', 'LineWidth', 1);
    % legend entries (drawn outside the visible range)
    h_gain = patch('XData', [-3 -2 -2 -3], 'YData', [0 0 0 0], 'FaceColor', C_BLUE, 'EdgeColor', 'k');
    h_loss = patch('XData', [-3 -2 -2 -3], 'YData', [0 0 0 0], 'FaceColor', C_VERMILLION, 'EdgeColor', 'k');
    legend([h_gain, h_loss], {'Net gain (+): tracking pays', 'Net loss (-): hatched bar'}, ...
           'Location', 'northoutside', 'Orientation', 'horizontal', 'FontSize', FONT_SIZE - 2);
    tick_txt = cell(1, NBARS);
    for b = 1:NBARS
        tick_txt{b} = datestr(day_no(b) + EPOCH_DATENUM, 'dd');
    end
    set(gca, 'XLim', [0.4, NBARS + 0.6], 'XTick', 1:NBARS, 'XTickLabel', tick_txt, 'YLim', y_lim);
    xlabel('UTC day of the month', 'FontSize', FONT_SIZE);
    ylabel('Net energy gain (%)', 'FontSize', FONT_SIZE);
    heading = {sprintf('Daily net energy gain, %s to %s (UTC)', ...
                       datestr(day_no(1) + EPOCH_DATENUM, 'dd mmm'), datestr(day_no(end) + EPOCH_DATENUM, 'dd mmm yyyy'))};
    if any(n_sim > 0), heading{end + 1} = 'sim = day includes SIMULATED samples'; end
    if any(complete < COMPLETE_MIN), heading{end + 1} = sprintf('part = day under %d %% complete', COMPLETE_MIN); end
    title(heading, 'FontSize', FONT_SIZE - 1);
    set(gca, 'YGrid', 'on');
    box on
end
hold off
