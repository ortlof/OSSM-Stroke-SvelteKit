<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { page } from '$app/state';
	import { user } from '$lib/stores/user';
	import { socket } from '$lib/stores/socket';
	import { notifications } from '$lib/components/toasts/notifications';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import Engine from '~icons/tabler/engine';
	import Play from '~icons/tabler/player-play';
	import Stop from '~icons/tabler/player-stop';
	import Home from '~icons/tabler/home';
	import Reload from '~icons/tabler/reload';

	interface StrokeState {
		run: boolean;
		keep_stroke_start: boolean;
		setup: number;
		speed: number;
		depth: number;
		stroke: number;
		sensation: number;
		pattern: number;
		servo_state: string;
		pattern_name: string;
		homed: boolean;
		position: number;
		speed_measured: number;
		physical_travel: number;
		usable_travel: number;
		sweep_start: number;
		clipping: boolean;
	}

	interface StrokeTuning {
		max_speed: number;
		user_speed_limit: number;
		reversal_acceleration_factor: number;
		home_margin: number;
	}

	interface PatternList {
		count: number;
		names: string[];
	}

	let state: StrokeState = $state({
		run: false,
		keep_stroke_start: false,
		setup: 0,
		speed: 20,
		depth: 0,
		stroke: 0,
		sensation: 0,
		pattern: 0,
		servo_state: 'UNDEFINED',
		pattern_name: '',
		homed: false,
		position: 0,
		speed_measured: 0,
		physical_travel: 450,
		usable_travel: 420,
		sweep_start: 0,
		clipping: false
	});

	let patterns: string[] = $state([]);
	let tuning: StrokeTuning = $state({
		max_speed: 700,
		user_speed_limit: 650,
		reversal_acceleration_factor: 0.7,
		home_margin: 15
	});
	// The engine confines depth and stroke to the travel minus both keepout
	// zones, so the sliders use that range, not the full measured travel.
	let travel = $derived(state.usable_travel > 0 ? state.usable_travel : 420);

	function authHeader() {
		return {
			Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
			'Content-Type': 'application/json'
		};
	}

	async function loadState() {
		try {
			const response = await fetch('/rest/strokeState', { method: 'GET', headers: authHeader() });
			if (response.ok) state = await response.json();
		} catch (error) {
			console.error('Error:', error);
		}
	}

	async function command(patch: Record<string, unknown>) {
		try {
			const response = await fetch('/rest/strokeState', {
				method: 'POST',
				headers: authHeader(),
				body: JSON.stringify(patch)
			});
			if (response.ok) {
				state = await response.json();
			} else {
				notifications.error('Command rejected (' + response.status + ')', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	async function loadTuning() {
		try {
			const response = await fetch('/rest/strokeSettings', { headers: authHeader() });
			if (response.ok) {
				const all = await response.json();
				tuning = {
					max_speed: all.max_speed,
					user_speed_limit: all.user_speed_limit,
					reversal_acceleration_factor: all.reversal_acceleration_factor,
					home_margin: all.home_margin
				};
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	async function saveTuning(patch: Partial<StrokeTuning>) {
		try {
			const response = await fetch('/rest/strokeSettings', {
				method: 'POST',
				headers: authHeader(),
				body: JSON.stringify(patch)
			});
			if (response.ok) {
				const all = await response.json();
				tuning = {
					max_speed: all.max_speed,
					user_speed_limit: all.user_speed_limit,
					reversal_acceleration_factor: all.reversal_acceleration_factor,
					home_margin: all.home_margin
				};
			} else {
				notifications.error('Setting rejected (' + response.status + ')', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	onMount(() => {
		loadState();
		loadTuning();
		// live telemetry, pushed by the engine synchronisation task
		socket.on<StrokeState>('stroke', (data) => {
			state = data;
		});
		// static pattern list, pushed on subscription
		socket.on<PatternList>('strokePatterns', (data) => {
			patterns = data.names ?? [];
		});
	});

	onDestroy(() => {
		socket.off('stroke');
		socket.off('strokePatterns');
	});
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Engine class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Stroke Engine</span>
	{/snippet}

	<div class="w-full space-y-4">
		<div class="stats stats-vertical shadow sm:stats-horizontal">
			<div class="stat">
				<div class="stat-title">Servo state</div>
				<div class="stat-value text-2xl">{state.servo_state}</div>
				<div class="stat-desc">{state.homed ? 'homed' : 'not homed'}</div>
			</div>
			<div class="stat">
				<div class="stat-title">Position</div>
				<div class="stat-value text-2xl">{state.position.toFixed(1)} mm</div>
				<div class="stat-desc">
					sweep {state.sweep_start.toFixed(0)} to {state.depth.toFixed(0)} mm of {travel.toFixed(0)} mm
				</div>
				<div class="stat-desc">
					speed {Math.abs(state.speed_measured).toFixed(0)} of {state.speed.toFixed(0)} mm/s
				</div>
			</div>
			<div class="stat">
				<div class="stat-title">Pattern</div>
				<div class="stat-value text-2xl">{state.pattern_name || '-'}</div>
				<div class="stat-desc">{state.clipping ? 'clipping active' : 'no clipping'}</div>
			</div>
		</div>

		<div class="flex flex-row flex-wrap gap-2">
			<button
				class="btn btn-primary inline-flex items-center"
				disabled={state.run}
				onclick={() => command({ run: true })}
			>
				<Play class="mr-2 h-5 w-5" /><span>Start</span>
			</button>
			<button
				class="btn btn-error inline-flex items-center"
				disabled={!state.run}
				onclick={() => command({ run: false })}
			>
				<Stop class="mr-2 h-5 w-5" /><span>Stop</span>
			</button>
			<button
				class="btn inline-flex items-center"
				class:btn-active={state.setup === 1}
				onclick={() => command({ setup: state.setup === 1 ? 0 : 1 })}
			>
				Setup depth
			</button>
			<button
				class="btn inline-flex items-center"
				class:btn-active={state.setup === 2}
				onclick={() => command({ setup: state.setup === 2 ? 0 : 2 })}
			>
				Setup depth fancy
			</button>
			<button class="btn inline-flex items-center" onclick={() => command({ home: true })}>
				<Home class="mr-2 h-5 w-5" /><span>Home</span>
			</button>
			<button class="btn inline-flex items-center" onclick={loadState}>
				<Reload class="mr-2 h-5 w-5" /><span>Reload</span>
			</button>
		</div>

		{#if state.setup !== 0}
			<div class="alert alert-warning my-2 shadow-lg">
				<span>
					Setup mode {state.setup === 2 ? 'fancy: the Sensation slider walks between depth minus stroke and depth' : 'depth: the Depth slider drives the machine'} at 10 mm/s. Press the button again, or press Start, to leave.
				</span>
			</div>
		{/if}

		<div class="divider">Motion</div>

		<div class="grid grid-cols-1 gap-x-8 gap-y-2 md:grid-cols-2">
			<div class="fieldset">
				<label class="label" for="speed">Speed (mm/s)</label>
				<input
					id="speed"
					type="range"
					class="range range-primary"
					min="1"
					max={travel}
					step="1"
					bind:value={state.speed}
					onchange={() => command({ speed: state.speed })}
				/>
				<div class="label">Speed is ramped, the engine never sees a step.</div>
			</div>

			<div class="fieldset">
				<label class="label" for="sensation">Sensation</label>
				<input
					id="sensation"
					type="range"
					class="range range-primary"
					min="-100"
					max="100"
					step="5"
					bind:value={state.sensation}
					onchange={() => command({ sensation: state.sensation })}
				/>
			</div>

			<div class="fieldset">
				<label class="label" for="depth">Depth ({state.depth.toFixed(0)} mm)</label>
				<input
					id="depth"
					type="range"
					class="range range-primary"
					min="0"
					max={travel}
					step="5"
					bind:value={state.depth}
					onchange={() => command({ depth: state.depth })}
				/>
			</div>

			<div class="fieldset">
				<label class="label" for="stroke">Stroke ({state.stroke.toFixed(0)} mm)</label>
				<input
					id="stroke"
					type="range"
					class="range range-primary"
					min="0"
					max={travel}
					step="5"
					bind:value={state.stroke}
					onchange={() => command({ stroke: state.stroke })}
				/>
			</div>

			<div class="fieldset">
				<label class="label cursor-pointer justify-start" for="keepstart">
					<input
						id="keepstart"
						type="checkbox"
						class="toggle toggle-primary mr-3"
						bind:checked={state.keep_stroke_start}
						onchange={() => command({ keep_stroke_start: state.keep_stroke_start })}
					/>
					<span class="text-base"
						>Keep the stroke start on depth change — a larger depth then makes a longer
						stroke</span
					>
				</label>
				<div class="label">
					Off: the whole sweep slides forward. On: the near end stays, the stroke grows.
				</div>
			</div>

			<div class="fieldset">
				<label class="label" for="pattern">Pattern</label>
				<select
					id="pattern"
					class="select select-primary w-full"
					bind:value={state.pattern}
					onchange={() => command({ pattern: Number(state.pattern) })}
				>
					{#each patterns as name, index (index)}
						<option value={index}>{name}</option>
					{/each}
				</select>
			</div>
		</div>

		<div class="divider">Tuning</div>

		<div class="grid grid-cols-1 gap-x-8 gap-y-2 md:grid-cols-2">
			<div class="fieldset">
				<label class="label" for="rev">Reversal acceleration factor ({tuning.reversal_acceleration_factor.toFixed(2)})</label>
				<input
					id="rev"
					type="range"
					class="range range-secondary"
					min="0.05"
					max="1"
					step="0.05"
					bind:value={tuning.reversal_acceleration_factor}
					onchange={() => saveTuning({ reversal_acceleration_factor: tuning.reversal_acceleration_factor })}
				/>
				<div class="label">
					Every stroke reverses, so this factor decides how much acceleration a turnaround gets. Higher is
					faster and louder; the gearbox backlash is what the low default protects.
				</div>
			</div>

			<div class="fieldset">
				<label class="label" for="margin">Keep this far off the home point ({tuning.home_margin.toFixed(0)} mm)</label>
				<input
					id="margin"
					type="range"
					class="range range-secondary"
					min="0"
					max="50"
					step="5"
					bind:value={tuning.home_margin}
					onchange={() => saveTuning({ home_margin: tuning.home_margin })}
				/>
				<div class="label">
					The near end of a sweep stays this far from the home position, so a Stroke can never be as long
					as the Depth. Distinct from the keepout the engine already applies: this one guards against the
					homing run stopping a little late against the end of the rail. 0 gives the full travel back.
				</div>
			</div>

			<div class="fieldset">
				<label class="label" for="limit">Speed limit for this slider ({tuning.user_speed_limit.toFixed(0)} mm/s)</label>
				<input
					id="limit"
					type="range"
					class="range range-secondary"
					min="50"
					max="700"
					step="10"
					bind:value={tuning.user_speed_limit}
					onchange={() => saveTuning({ user_speed_limit: tuning.user_speed_limit })}
				/>
				<div class="label">
					The highest value the Speed slider can reach. The measured peak stays below it, more so with a
					short stroke: about 100 % at 200 mm, 78 % at 60 mm.
				</div>
			</div>

			<div class="fieldset">
				<label class="label" for="maxspeed">Hard ceiling ({tuning.max_speed.toFixed(0)} mm/s)</label>
				<input
					id="maxspeed"
					type="range"
					class="range range-secondary"
					min="100"
					max="800"
					step="10"
					bind:value={tuning.max_speed}
					onchange={() => saveTuning({ max_speed: tuning.max_speed })}
				/>
				<div class="label">
					The engine refuses to plan faster than this. It is read when the motion driver is built, so a
					change needs a restart. 650 mm/s is about 4150 rpm at the motor.
				</div>
			</div>
		</div>
	</div>
</SettingsCard>
