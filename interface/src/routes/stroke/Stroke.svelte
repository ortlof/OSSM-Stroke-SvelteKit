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

	onMount(() => {
		loadState();
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
	</div>
</SettingsCard>
